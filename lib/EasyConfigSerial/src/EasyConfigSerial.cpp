#include "EasyConfigSerial.h"

#include "EasyConfigBase64.h"
#include <algorithm>
#include <utility>

namespace {
#if ARDUINOJSON_VERSION_MAJOR < 7
using ProtocolDocument = DynamicJsonDocument;
// Allow dense arrays as well as strings; v6 requires an explicit pool size.
size_t documentCapacity(size_t bytes) {
  return JSON_ARRAY_SIZE(bytes / 2 + 1) + bytes + 1024;
}
#else
class ProtocolDocument : public JsonDocument {
 public:
  explicit ProtocolDocument(size_t) {}
};
size_t documentCapacity(size_t) { return 0; }
#endif
constexpr const char *PREFIX = "@EC1:";
constexpr size_t MAX_LINE = 1024;
constexpr uint32_t TRANSFER_TIMEOUT_MS = 30000;

struct JsonReader {
  const String &text;
  size_t position = 0;
  int read() { return position < text.length() ? uint8_t(text[position++]) : -1; }
  size_t readBytes(char *out, size_t length) {
    size_t count = 0;
    while (count < length && position < text.length()) out[count++] = text[position++];
    return count;
  }
};
bool whitespace(char c) { return c == ' ' || c == '\r' || c == '\n' || c == '\t'; }
}

EasyConfigSerial::EasyConfigSerial(Stream &serial, size_t maxBytes)
    : serial_(serial), maxBytes_(maxBytes) {}

void EasyConfigSerial::begin(const char *device, Backend backend) {
  device_ = device;
  backend_ = std::move(backend);
  line_.reserve(MAX_LINE);
  resetTransfer();
}

String EasyConfigSerial::crc32(const String &content) {
  uint32_t crc = 0xFFFFFFFF;
  for (size_t i = 0; i < content.length(); ++i) {
    crc ^= uint8_t(content[i]);
    for (unsigned bit = 0; bit < 8; ++bit)
      crc = (crc >> 1) ^ (0xEDB88320U & (0U - (crc & 1U)));
  }
  char out[9];
  snprintf(out, sizeof(out), "%08lx", static_cast<unsigned long>(crc ^ 0xFFFFFFFF));
  return String(out);
}

String EasyConfigSerial::revision(const String &content, bool exists) {
  return exists ? String(content.length()) + ":" + crc32(content) : String("missing");
}

bool EasyConfigSerial::validateJson(const String &content, String &error) {
  for (size_t i = 0; i < content.length(); ++i) {
    if (content[i] == '\0') { error = "JSON contains a NUL byte"; return false; }
  }
  JsonReader reader{content};
  ProtocolDocument doc(documentCapacity(content.length()));
  const auto result = deserializeJson(doc, reader);
  if (result) { error = String("Invalid JSON: ") + result.c_str(); return false; }
  if (!doc.is<JsonObject>()) { error = "JSON root must be an object"; return false; }
  while (reader.position < content.length()) {
    if (!whitespace(content[reader.position++])) {
      error = "Unexpected content after JSON object";
      return false;
    }
  }
  error = "";
  return true;
}

void EasyConfigSerial::resetTransfer() {
  mode_ = Mode::Idle;
  // Move an empty string in to release the potentially large transfer buffer.
  buffer_ = String();
  expectedCrc_ = "";
  baseRevision_ = "";
  expectedBytes_ = 0;
}

void EasyConfigSerial::poll() {
  const uint32_t now = millis();
  if (restartPending_ && int32_t(now - restartAtMs_) >= 0) {
    restartPending_ = false;
    if (backend_.restart) backend_.restart();
  }
  if (mode_ != Mode::Idle && now - transferAtMs_ > TRANSFER_TIMEOUT_MS) resetTransfer();
  if (line_.length() && now - lastRxMs_ > 3000) {
    line_ = "";
    discarding_ = true;
  }
  // Bound work per loop and use stop-and-wait chunks smaller than the RX buffer.
  for (size_t budget = 0; budget < MAX_LINE && serial_.available(); ++budget) {
    const int value = serial_.read();
    if (value < 0) break;
    lastRxMs_ = millis();
    const char c = char(value);
    if (c == '\n') {
      if (!discarding_ && line_.startsWith(PREFIX)) handle(line_);
      line_ = "";
      discarding_ = false;
      break;
    }
    if (discarding_ || c == '\r') continue;
    if (line_.length() >= MAX_LINE || !line_.concat(c)) {
      line_ = "";
      discarding_ = true;
    }
  }
}

void EasyConfigSerial::respond(JsonDocument &response) {
  if (response.overflowed()) return; // Host times out; never send a partial success.
  String frame;
  const size_t bytes = measureJson(response);
  if (!frame.reserve(bytes + 8)) return;
  // ArduinoJson 7 clears String destinations before serializing.
  if (serializeJson(response, frame) != bytes) return;
  frame = String("\n@EC1:") + frame + "\n";
  if (frame.length() != bytes + 7) return;
  // A single write uses the ESP32 HardwareSerial TX lock for the entire frame.
  serial_.write(reinterpret_cast<const uint8_t *>(frame.c_str()), frame.length());
}

bool EasyConfigSerial::checkUpload(String &error) {
  if (mode_ != Mode::Write) { error = "No upload; start again"; return false; }
  if (buffer_.length() != expectedBytes_) { error = "Upload incomplete"; return false; }
  if (crc32(buffer_) != expectedCrc_) { error = "CRC32 mismatch; upload again"; return false; }
  if (!validateJson(buffer_, error)) return false;
  if (!backend_.validate) { error = "Validation backend unavailable"; return false; }
  if (!backend_.validate(buffer_, error)) {
    if (error.isEmpty()) error = "Config validation failed";
    return false;
  }
  return true;
}

void EasyConfigSerial::handle(const String &line) {
  ProtocolDocument request(documentCapacity(line.length()));
  if (deserializeJson(request, line.c_str() + 5) || !request["id"].is<uint32_t>()) return;
  ProtocolDocument response(documentCapacity(0) + maxBytes_);
  response["id"] = request["id"].as<uint32_t>();
  response["ok"] = false;
  const String op = request["op"] | "";
  String error;
  transferAtMs_ = millis();

  if (op == "hello") {
    resetTransfer();
    response["protocol"] = 1;
    response["device"] = device_;
    // Resolve identity from persisted config on every handshake, including
    // after a serial/web save. A damaged config must not prevent connection.
    String content;
    String readError;
    bool exists = false;
    if (backend_.read && backend_.read(content, exists, readError) && exists &&
        content.length() <= maxBytes_ && validateJson(content, readError)) {
      ProtocolDocument config(documentCapacity(content.length()));
      if (!deserializeJson(config, content) && config["id"].is<const char *>()) {
        const String id = config["id"].as<String>();
        for (size_t i = 0; i < id.length(); ++i) {
          if (!whitespace(id[i])) {
            response["device"] = id;
            break;
          }
        }
      }
    }
    response["path"] = "/config.json";
    response["maxBytes"] = maxBytes_;
    response["chunkBytes"] = CHUNK_BYTES;
    response["restartSupported"] = bool(backend_.restart);
  } else if (op == "read.begin") {
    resetTransfer();
    bool exists = false;
    if (!backend_.read || !backend_.read(buffer_, exists, error)) {
      if (error.isEmpty()) error = "Read backend unavailable";
    } else if (buffer_.length() > maxBytes_) {
      error = "Config exceeds device size limit";
    } else {
      mode_ = Mode::Read;
      response["exists"] = exists;
      response["bytes"] = buffer_.length();
      response["crc"] = crc32(buffer_);
      response["revision"] = revision(buffer_, exists);
    }
    if (!error.isEmpty()) resetTransfer();
  } else if (op == "read.chunk") {
    const size_t offset = request["offset"] | size_t(-1);
    if (mode_ != Mode::Read || offset > buffer_.length()) {
      error = "Invalid read offset or expired snapshot";
    } else {
      const size_t count = std::min(CHUNK_BYTES, buffer_.length() - offset);
      unsigned char encoded[CHUNK_BYTES * 4 / 3 + 1] = {};
      size_t length = 0;
      easyconfig::encode(encoded, sizeof(encoded), &length,
                           reinterpret_cast<const unsigned char *>(buffer_.c_str()) + offset, count);
      response["offset"] = offset;
      response["data"] = reinterpret_cast<const char *>(encoded);
    }
  } else if (op == "write.begin") {
    resetTransfer();
    const size_t bytes = request["bytes"] | size_t(0);
    const String crc = request["crc"] | "";
    const String base = request["revision"] | "";
    if (!bytes || bytes > maxBytes_ || crc.length() != 8 || base.isEmpty()) {
      error = "Invalid upload length, checksum, or base revision";
    } else if (!buffer_.reserve(bytes)) {
      error = "Not enough free RAM for upload";
    } else {
      mode_ = Mode::Write;
      expectedBytes_ = bytes;
      expectedCrc_ = crc;
      baseRevision_ = base;
      response["offset"] = 0;
    }
  } else if (op == "write.chunk") {
    const size_t offset = request["offset"] | size_t(-1);
    const char *data = request["data"] | "";
    unsigned char decoded[CHUNK_BYTES];
    size_t count = 0;
    if (mode_ != Mode::Write || offset != buffer_.length()) {
      error = "Unexpected upload offset; start again";
    } else if (easyconfig::decode(decoded, sizeof(decoded), &count,
                                   reinterpret_cast<const unsigned char *>(data), strlen(data)) ||
               !count || count > expectedBytes_ - buffer_.length()) {
      error = "Invalid upload chunk";
    } else if (!buffer_.concat(reinterpret_cast<const char *>(decoded), count)) {
      error = "Not enough free RAM";
    } else {
      response["offset"] = buffer_.length();
    }
  } else if (op == "validate" || op == "commit") {
    if (checkUpload(error) && op == "commit") {
      String current;
      bool exists = false;
      if (!backend_.read || !backend_.read(current, exists, error)) {
        if (error.isEmpty()) error = "Unable to check current revision";
      } else if (revision(current, exists) != baseRevision_) {
        error = "Config changed on device; read it again before saving";
      } else {
        current = String();
        if (!backend_.write || !backend_.write(buffer_, error)) {
          if (error.isEmpty()) error = "Save backend unavailable";
        } else {
          response["saved"] = true;
          response["restartRequired"] = true;
          resetTransfer();
        }
      }
    }
  } else if (op == "abort" || op == "read.end") {
    resetTransfer();
  } else if (op == "restart") {
    if (!backend_.restart) error = "Restart unsupported";
    else { resetTransfer(); restartPending_ = true; restartAtMs_ = millis() + 500; }
  } else {
    error = "Unknown operation";
  }
  response["ok"] = error.isEmpty();
  if (!error.isEmpty()) response["error"] = error;
  respond(response);
}
