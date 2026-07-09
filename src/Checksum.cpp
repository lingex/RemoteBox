#include "Checksum.h"

namespace {

constexpr size_t MAX_CANONICAL_OBJECT_KEYS = 24;
// Each object level keeps a stack-allocated member array, so the recursion
// depth must stay bounded to protect the (small) ESP task stack.
constexpr size_t MAX_CANONICAL_DEPTH = 8;

// A member reference into the source document. Both fields point into memory
// owned by the JsonDocument, so copying/sorting these never touches the heap.
struct CanonicalJsonMember {
  const char *key;
  JsonVariantConst value;
};

// A Print sink that folds every byte written to it straight into a running
// CRC32, so the canonical form is never materialized in RAM.
class CrcPrint : public Print {
 public:
  uint32_t crc = 0xFFFFFFFFUL;

  size_t write(uint8_t data) override {
    crc ^= data;
    for (uint8_t i = 0; i < 8; ++i) {
      crc = (crc & 1) ? (crc >> 1) ^ 0xEDB88320UL : crc >> 1;
    }
    return 1;
  }

  size_t write(const uint8_t *buffer, size_t size) override {
    for (size_t i = 0; i < size; ++i) {
      write(buffer[i]);
    }
    return size;
  }
};

void appendJsonString(const char *value, Print &output) {
  output.write(static_cast<uint8_t>('"'));
  for (const char *p = value; *p; ++p) {
    const char c = *p;
    switch (c) {
      case '"':
        output.write("\\\"");
        break;
      case '\\':
        output.write("\\\\");
        break;
      case '\b':
        output.write("\\b");
        break;
      case '\f':
        output.write("\\f");
        break;
      case '\n':
        output.write("\\n");
        break;
      case '\r':
        output.write("\\r");
        break;
      case '\t':
        output.write("\\t");
        break;
      default:
        if (static_cast<uint8_t>(c) < 0x20) {
          char escaped[7];
          snprintf(escaped, sizeof(escaped), "\\u%04X", static_cast<unsigned int>(static_cast<uint8_t>(c)));
          output.write(escaped);
        } else {
          output.write(static_cast<uint8_t>(c));
        }
        break;
    }
  }
  output.write(static_cast<uint8_t>('"'));
}

void appendCanonicalJson(JsonVariantConst value, Print &output, bool &failed, size_t depth) {
  if (failed) return;

  if (value.is<JsonObjectConst>()) {
    if (depth >= MAX_CANONICAL_DEPTH) {
      failed = true;
      return;
    }
    JsonObjectConst object = value.as<JsonObjectConst>();
    CanonicalJsonMember members[MAX_CANONICAL_OBJECT_KEYS];
    size_t memberCount = 0;

    for (JsonPairConst pair : object) {
      const char *key = pair.key().c_str();
      if (depth == 0 && strcmp(key, "chk") == 0) continue;
      if (memberCount >= MAX_CANONICAL_OBJECT_KEYS) {
        failed = true;
        return;
      }
      members[memberCount++] = {key, pair.value()};
    }

    for (size_t i = 1; i < memberCount; ++i) {
      const CanonicalJsonMember member = members[i];
      size_t j = i;
      while (j > 0 && strcmp(members[j - 1].key, member.key) > 0) {
        members[j] = members[j - 1];
        --j;
      }
      members[j] = member;
    }

    output.write(static_cast<uint8_t>('{'));
    for (size_t i = 0; i < memberCount; ++i) {
      if (i > 0) output.write(static_cast<uint8_t>(','));
      appendJsonString(members[i].key, output);
      output.write(static_cast<uint8_t>(':'));
      appendCanonicalJson(members[i].value, output, failed, depth + 1);
      if (failed) return;
    }
    output.write(static_cast<uint8_t>('}'));
    return;
  }

  if (value.is<JsonArrayConst>()) {
    if (depth >= MAX_CANONICAL_DEPTH) {
      failed = true;
      return;
    }
    JsonArrayConst array = value.as<JsonArrayConst>();
    output.write(static_cast<uint8_t>('['));
    bool first = true;
    for (JsonVariantConst item : array) {
      if (!first) output.write(static_cast<uint8_t>(','));
      first = false;
      appendCanonicalJson(item, output, failed, depth + 1);
      if (failed) return;
    }
    output.write(static_cast<uint8_t>(']'));
    return;
  }

  serializeJson(value, output);
}

}  // namespace

String commandChecksum(const JsonDocument &doc) {
  CrcPrint sink;
  bool failed = false;
  appendCanonicalJson(doc.as<JsonVariantConst>(), sink, failed, 0);
  if (failed) return String();

  const uint32_t crc = sink.crc ^ 0xFFFFFFFFUL;
  char output[9];
  snprintf(output, sizeof(output), "%08lX", static_cast<unsigned long>(crc));
  return String(output);
}
