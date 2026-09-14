#include "EasyConfigFile.h"
#include <utility>
#include <algorithm>

EasyConfigFile::EasyConfigFile(fs::FS &fs, const char *path, size_t maxBytes)
    : fs_(fs), path_(path), maxBytes_(maxBytes) {}

EasyConfigSerial::Backend EasyConfigFile::backend(std::function<void()> restart) {
  EasyConfigSerial::Backend result;
  result.read = [this](String &text, bool &exists, String &error) { return read(text, exists, error); };
  result.validate = [this](const String &text, String &error) { return validate(text, error); };
  result.write = [this](const String &text, String &error) { return write(text, error); };
  result.restart = std::move(restart);
  return result;
}

bool EasyConfigFile::read(String &content, bool &exists, String &error) {
  content = "";
  exists = false;
  if (!mounted_) { error = "LittleFS is not mounted; no files were changed"; return false; }
  exists = fs_.exists(path_);
  if (!exists) { error = ""; return true; }
  File file = fs_.open(path_, "r");
  if (!file) { error = "Cannot open config file"; return false; }
  const size_t size = file.size();
  if (size > maxBytes_) { error = "Config exceeds device size limit"; return false; }
  content = file.readString();
  if (content.length() != size) { error = "Incomplete config read"; return false; }
  error = "";
  return true;
}

bool EasyConfigFile::validate(const String &content, String &error) {
  if (content.isEmpty() || content.length() > maxBytes_) {
    error = "Config is empty or exceeds device size limit";
    return false;
  }
  if (!EasyConfigSerial::validateJson(content, error)) return false;
  return !validator_ || validator_(content, error);
}

bool EasyConfigFile::verifiedWrite(const String &path, const String &content, String &error) {
  File file = fs_.open(path, "w");
  if (!file) { error = "Cannot open temporary file"; return false; }
  const size_t written = file.print(content);
  file.flush();
  file.close();
  File check = fs_.open(path, "r");
  bool equal = check && written == content.length() && check.size() == content.length();
  // Verify every byte with bounded memory, including flush/close failures.
  uint8_t chunk[128];
  for (size_t offset = 0; equal && offset < content.length();) {
    const size_t count = std::min(sizeof(chunk), content.length() - offset);
    equal = check.read(chunk, count) == count && !memcmp(chunk, content.c_str() + offset, count);
    offset += count;
  }
  check.close();
  if (!equal) { fs_.remove(path); error = "File write verification failed"; return false; }
  return true;
}

bool EasyConfigFile::write(const String &content, String &error) {
  if (!mounted_) { error = "LittleFS is not mounted"; return false; }
  if (!validate(content, error)) return false;
  const String temp = path_ + ".ec-tmp";
  const String backupTemp = path_ + ".ec-bak-tmp";
  const String backup = path_ + ".ec-bak";
  String previous;
  bool exists;
  if (!read(previous, exists, error)) return false;
  if (exists) {
    if (!verifiedWrite(backupTemp, previous, error)) return false;
    if (!fs_.rename(backupTemp, backup)) { error = "Unable to install backup"; return false; }
  }
  previous = String();
  if (!verifiedWrite(temp, content, error)) return false;
  // LittleFS rename replaces an existing file atomically. Never remove/move the
  // live config first: power loss must leave either the old or the new config.
  if (!fs_.rename(temp, path_)) { error = "Unable to install config"; return false; }
  error = "";
  return true;
}
