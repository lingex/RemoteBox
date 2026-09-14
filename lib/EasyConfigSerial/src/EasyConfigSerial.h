#pragma once

#include <Arduino.h>
#include <ArduinoJson.h>
#include <functional>

// Call begin()/poll() and all backend operations on the same task. This class
// owns serial RX; application logs may share TX. No WiFi or filesystem dependency.
class EasyConfigSerial {
 public:
  struct Backend {
    // Missing file: return true, exists=false, content="". I/O failure: false.
    std::function<bool(String &content, bool &exists, String &error)> read;
    std::function<bool(const String &content, String &error)> validate;
    // Must validate again and atomically install the file. Do not reboot here.
    std::function<bool(const String &content, String &error)> write;
    std::function<void()> restart;
  };

  static constexpr size_t CHUNK_BYTES = 384;
  explicit EasyConfigSerial(Stream &serial, size_t maxBytes = 16384);
  // device is the project-name fallback; hello prefers the stored JSON's id.
  void begin(const char *device, Backend backend);
  void poll();
  static String crc32(const String &content);
  static String revision(const String &content, bool exists);
  static bool validateJson(const String &content, String &error);

 private:
  enum class Mode { Idle, Read, Write };
  void handle(const String &line);
  void resetTransfer();
  void respond(JsonDocument &response);
  bool checkUpload(String &error);

  Stream &serial_;
  const size_t maxBytes_;
  Backend backend_;
  String device_;
  String line_;
  String buffer_;
  String expectedCrc_;
  String baseRevision_;
  size_t expectedBytes_ = 0;
  uint32_t lastRxMs_ = 0;
  uint32_t transferAtMs_ = 0;
  uint32_t restartAtMs_ = 0;
  bool restartPending_ = false;
  bool discarding_ = false;
  Mode mode_ = Mode::Idle;
};
