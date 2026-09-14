#include <Arduino.h>
#include <LittleFS.h>
#include <EasyConfigFile.h>

EasyConfigSerial maintenance(Serial);
EasyConfigFile configFile(LittleFS);

void setup() {
  Serial.setRxBufferSize(2048); // Before begin(); an encoded chunk is ~600 bytes.
  Serial.begin(115200);
#ifdef ESP8266
  LittleFSConfig fsConfig;
  fsConfig.setAutoFormat(false);
  LittleFS.setConfig(fsConfig);
  configFile.setMounted(LittleFS.begin());
#else
  configFile.setMounted(LittleFS.begin(false));
#endif
  configFile.setValidator([](const String &content, String &error) {
#if ARDUINOJSON_VERSION_MAJOR < 7
    DynamicJsonDocument doc(8192);
#else
    JsonDocument doc;
#endif
    if (deserializeJson(doc, content)) { error = "Invalid JSON"; return false; }
    if (!doc["wifi"]["ssid"].is<const char *>() ||
        !doc["wifi"]["password"].is<const char *>()) {
      error = "wifi.ssid and wifi.password must be strings";
      return false;
    }
    if (doc["wifi"]["ssid"].as<String>().length() > 32 ||
        doc["wifi"]["password"].as<String>().length() > 64) {
      error = "WiFi field is too long";
      return false;
    }
    error = "";
    return true;
  });
  maintenance.begin("ESP32", configFile.backend([] { ESP.restart(); }));
  // Load project settings next. On invalid JSON use defaults and keep polling.
  // Poll maintenance inside any blocking WiFi connection loop as well.
}

void loop() {
  maintenance.poll();
  delay(1);
}
