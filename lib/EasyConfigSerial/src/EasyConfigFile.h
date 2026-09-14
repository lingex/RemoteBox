#pragma once

#include "EasyConfigSerial.h"
#include <FS.h>
#include <utility>

// Optional backend for projects without their own ConfigStore. Mount LittleFS
// yourself with formatOnFail=false, then call setMounted(mountSucceeded).
class EasyConfigFile {
 public:
  using Validator = std::function<bool(const String &, String &)>;
  explicit EasyConfigFile(fs::FS &fs, const char *path = "/config.json",
                          size_t maxBytes = 16384);
  void setMounted(bool mounted) { mounted_ = mounted; }
  void setValidator(Validator validator) { validator_ = std::move(validator); }
  EasyConfigSerial::Backend backend(std::function<void()> restart = nullptr);
  bool read(String &content, bool &exists, String &error);
  bool validate(const String &content, String &error);
  bool write(const String &content, String &error);

 private:
  bool verifiedWrite(const String &path, const String &content, String &error);
  fs::FS &fs_;
  String path_;
  size_t maxBytes_;
  bool mounted_ = false;
  Validator validator_;
};
