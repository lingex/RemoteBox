#pragma once

#include "EasyConfigFile.h"

// Own this in the same task as the application's other filesystem users.
class EasyConfigMaintenance {
 public:
  EasyConfigMaintenance(Stream &stream, fs::FS &fs, size_t maxBytes = 8192)
      : file_(fs, "/config.json", maxBytes), protocol_(stream, maxBytes) {}

  void begin(const char *project, bool mounted, EasyConfigFile::Validator validator,
             std::function<void()> restart) {
    file_.setMounted(mounted);
    file_.setValidator(std::move(validator));
    auto backend = file_.backend(std::move(restart));
    backend.write = [this](const String &content, String &error) {
      if (!file_.write(content, error)) return false;
      restartRequired_ = true;
      return true;
    };
    protocol_.begin(project, std::move(backend));
    started_ = true;
  }
  void poll() { if (started_) protocol_.poll(); }
  void setMounted(bool mounted) { file_.setMounted(mounted); }
  bool restartRequired() const { return restartRequired_; }

 private:
  EasyConfigFile file_;
  EasyConfigSerial protocol_;
  bool started_ = false;
  bool restartRequired_ = false;
};
