#include <EasyConfigMaintenance.h>
#include <Arduino.h>
#include <ArduinoOTA.h>
#include <ArduinoJson.h>
#include <ESPmDNS.h>
#include <FS.h>
#include <LittleFS.h>
#include <U8g2lib.h>
#include <WebServer.h>
#include <WiFi.h>
#include <esp_now.h>
#include <esp_sleep.h>
#include <esp_wifi.h>
#include <driver/gpio.h>

#include "Checksum.h"
#include "WebPage.h"

EasyConfigMaintenance serialConfig(Serial, LittleFS, 4096);

#ifndef ESP_ARDUINO_VERSION_MAJOR
#define ESP_ARDUINO_VERSION_MAJOR 2
#endif

namespace {

constexpr uint8_t PIN_PWR_EN = 1;
constexpr uint8_t PIN_BUTTON = 8;
constexpr uint8_t PIN_USB_DET = 10;
constexpr uint8_t PIN_LCD_SCK = 6;
constexpr uint8_t PIN_LCD_SDA = 7;
constexpr uint8_t PIN_LCD_DC = 5;
constexpr uint8_t PIN_LCD_RST = 4;
constexpr uint8_t PIN_LCD_BL = 9;
constexpr uint8_t PIN_BAT_EN = 0;
constexpr uint8_t PIN_BAT_VAL = 3;
constexpr uint8_t LCD_BL_ON_LEVEL = LOW;
constexpr uint8_t LCD_BL_PWM_CHANNEL = 0;
constexpr uint32_t LCD_BL_PWM_FREQ = 5000;
constexpr uint8_t LCD_BL_PWM_BITS = 8;
constexpr uint8_t LCD_BL_PWM_MAX = (1 << LCD_BL_PWM_BITS) - 1;

constexpr char CONFIG_PATH[] = "/config.json";
constexpr char CONFIG_TEMP_PATH[] = "/.config.tmp";
constexpr char CONFIG_BACKUP_PATH[] = "/.config.bak";
constexpr char NETWORK_HOSTNAME[] = "remotebox";
constexpr char DEFAULT_ID[] = "remote-001";
constexpr char DEFAULT_NAME[] = "RemoteBox";
constexpr char DEFAULT_TO[] = "IRStation-01";
constexpr char DEFAULT_CMD[] = "power";
constexpr char DEFAULT_DATA_JSON[] = "{}";
constexpr char DEFAULT_WIFI_SSID[] = "";
constexpr char DEFAULT_WIFI_PASSWORD[] = "";
constexpr uint8_t AUTO_CHANNEL = 0;
constexpr uint8_t DEFAULT_CHANNEL = 1;
constexpr uint8_t MIN_CHANNEL = 1;
constexpr uint8_t MAX_CHANNEL = 13;
constexpr uint8_t DEFAULT_BACKLIGHT_BRIGHTNESS = 50;
constexpr size_t MAX_ID_LEN = 32;
constexpr size_t MAX_NAME_LEN = 32;
constexpr size_t MAX_TO_LEN = 32;
constexpr size_t MAX_CMD_LEN = 32;
constexpr size_t MAX_DATA_JSON_LEN = 96;
constexpr size_t MAX_WIFI_SSID_LEN = 32;
constexpr size_t MAX_WIFI_PASSWORD_LEN = 64;
constexpr size_t ESPNOW_PAYLOAD_MAX_LEN = 250;
constexpr size_t MAX_CONFIG_FILE_LEN = 4096;

constexpr uint32_t RESULT_VISIBLE_MS = 3UL * 1000UL;
constexpr uint32_t USB_STATUS_VISIBLE_MS = 3UL * 1000UL;
constexpr uint32_t USB_BACKLIGHT_IDLE_MS = 9UL * 1000UL;
constexpr uint32_t POST_BACKLIGHT_OFF_MS = 2UL * 1000UL;
constexpr uint32_t USB_REFRESH_MS = 700UL;
constexpr uint32_t BUTTON_DEBOUNCE_MS = 35UL;
constexpr uint32_t BUTTON_LONG_PRESS_MS = 2UL * 1000UL;
constexpr uint32_t CHANNEL_SETTINGS_TIMEOUT_MS = 5UL * 1000UL;
constexpr uint32_t CHANNEL_SETTINGS_RESULT_MS = 1UL * 1000UL;
constexpr uint32_t BATTERY_REFRESH_MS = 8UL * 1000UL;
constexpr uint8_t COMMAND_SEND_ATTEMPTS = 3;
constexpr uint32_t ESPNOW_SEND_WAIT_MS = 500UL;
constexpr uint32_t ESPNOW_SEND_GAP_MS = 35UL;
constexpr uint32_t WIFI_CONNECT_TIMEOUT_MS = 15UL * 1000UL;
constexpr uint32_t WIFI_RETRY_MS = 60UL * 1000UL;
constexpr uint32_t WIFI_RESTART_DELAY_MS = 800UL;
constexpr uint32_t REBOOT_DELAY_MS = 500UL;
constexpr float BATTERY_DIVIDER_RATIO = 2.0f;

constexpr uint8_t ESPNOW_BROADCAST_MAC[6] = {
    0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};

struct WifiConfig {
  String ssid = DEFAULT_WIFI_SSID;
  String password = DEFAULT_WIFI_PASSWORD;
};

struct DeviceConfig {
  WifiConfig wifi;
  String id = DEFAULT_ID;
  String name = DEFAULT_NAME;
  String to = DEFAULT_TO;
  String cmd = DEFAULT_CMD;
  String dataJson = DEFAULT_DATA_JSON;
  uint8_t channel = DEFAULT_CHANNEL;
  uint8_t backlightBrightness = DEFAULT_BACKLIGHT_BRIGHTNESS;
};

struct BatteryReading {
  uint16_t millivolts = 0;
  uint8_t percent = 0;
};

struct BatteryCurvePoint {
  uint16_t millivolts;
  uint8_t percent;
};

constexpr BatteryCurvePoint BATTERY_CURVE[] = {
    {3300, 0},  {3500, 10}, {3590, 20}, {3650, 30}, {3700, 40},
    {3760, 50}, {3830, 60}, {3920, 70}, {4020, 80}, {4110, 90},
    {4150, 100},
};

enum class SendResult {
  Ok,
  Timeout,
  Error,
};

enum class ButtonEvent {
  None,
  Pressed,
  Released,
};

enum class WifiChannelScanStatus {
  Found,
  NotFound,
  Error,
};

struct WifiChannelScanResult {
  WifiChannelScanStatus status = WifiChannelScanStatus::Error;
  uint8_t channel = AUTO_CHANNEL;
};

DeviceConfig config;
BatteryReading battery;
U8G2_ST7567_ERC12864_F_4W_SW_SPI u8g2(
    U8G2_R0, PIN_LCD_SCK, PIN_LCD_SDA, U8X8_PIN_NONE, PIN_LCD_DC, PIN_LCD_RST);
WebServer webServer(80);

bool usbPresent = false;
bool backlightOn = false;
bool espNowInitialized = false;
bool wifiRadioStarted = false;
bool wifiAttemptActive = false;
bool wifiRestartPending = false;
bool webRoutesConfigured = false;
bool webServerStarted = false;
bool mdnsStarted = false;
bool arduinoOtaConfigured = false;
bool arduinoOtaStarted = false;
bool otaInProgress = false;
bool rebootPending = false;
uint8_t otaLastProgress = UINT8_MAX;
bool lastButtonRaw = false;
bool stableButtonPressed = false;
bool buttonLongHandled = false;
uint32_t buttonChangedAtMs = 0;
uint32_t buttonPressStartedMs = 0;
uint32_t lastBatteryRefreshMs = 0;
uint32_t lastUsbRefreshMs = 0;
uint32_t lastUsbActivityMs = 0;
uint8_t batteryChargeFrame = 0;
uint16_t commandSequence = 0;
String usbStatusLine;
uint32_t usbStatusUntilMs = 0;
uint32_t wifiAttemptStartedMs = 0;
uint32_t nextWifiAttemptMs = 0;
uint32_t wifiRestartAtMs = 0;
uint32_t nextChannelSyncAttemptMs = 0;
uint32_t rebootAtMs = 0;

volatile bool espNowSendComplete = false;
volatile esp_now_send_status_t espNowLastStatus = ESP_NOW_SEND_FAIL;

void setupButtonState();
bool hasCompleteWifiConfig();
void stopNetworkServices();
void serviceNetwork();

bool elapsed(uint32_t now, uint32_t since, uint32_t interval) {
  return static_cast<uint32_t>(now - since) >= interval;
}

String limitString(const String &value, size_t maxLen, const char *fallback) {
  String trimmed = value;
  trimmed.trim();
  if (trimmed.isEmpty()) {
    trimmed = fallback;
  }
  if (trimmed.length() > maxLen) {
    trimmed.remove(maxLen);
  }
  return trimmed;
}

String configStringOrEmpty(JsonVariantConst value, size_t maxLen,
                           bool &changed) {
  if (!value.is<const char *>()) {
    changed = true;
    return "";
  }

  String output = value.as<const char *>();
  if (output.length() > maxLen) {
    output.remove(maxLen);
    changed = true;
  }
  return output;
}

uint8_t clampPercent(int value, uint8_t fallback, bool &changed) {
  if (value < 0 || value > 100) {
    changed = true;
    return fallback;
  }
  return static_cast<uint8_t>(value);
}

String compactJsonOrDefault(JsonVariantConst value, const char *fallback,
                            size_t maxLen, bool &changed) {
  String output;
  if (value.isNull()) {
    output = fallback;
    changed = true;
  } else {
    serializeJson(value, output);
    if (output.isEmpty() || output.length() > maxLen) {
      output = fallback;
      changed = true;
    }
  }
  return output;
}

uint8_t backlightDutyForBrightness(uint8_t brightness) {
  const uint8_t duty =
      (static_cast<uint16_t>(brightness) * LCD_BL_PWM_MAX + 50) / 100;
  return LCD_BL_ON_LEVEL == LOW ? LCD_BL_PWM_MAX - duty : duty;
}

void writeBacklightOutput(uint8_t duty) {
#if ESP_ARDUINO_VERSION_MAJOR >= 3
  ledcWrite(PIN_LCD_BL, duty);
#else
  ledcWrite(LCD_BL_PWM_CHANNEL, duty);
#endif
}

void setupBacklightPwm() {
#if ESP_ARDUINO_VERSION_MAJOR >= 3
  ledcAttach(PIN_LCD_BL, LCD_BL_PWM_FREQ, LCD_BL_PWM_BITS);
#else
  ledcSetup(LCD_BL_PWM_CHANNEL, LCD_BL_PWM_FREQ, LCD_BL_PWM_BITS);
  ledcAttachPin(PIN_LCD_BL, LCD_BL_PWM_CHANNEL);
#endif
}

void setBacklight(bool enabled) {
  backlightOn = enabled;
  writeBacklightOutput(enabled ? backlightDutyForBrightness(
                                     config.backlightBrightness)
                               : backlightDutyForBrightness(0));
}

bool readUsbPresent() {
  return digitalRead(PIN_USB_DET) == HIGH;
}

bool readButtonPressed() {
  return digitalRead(PIN_BUTTON) == HIGH;
}

void lockPower() {
  pinMode(PIN_PWR_EN, OUTPUT);
  digitalWrite(PIN_PWR_EN, HIGH);
}

void holdPowerOff() {
  gpio_hold_dis(static_cast<gpio_num_t>(PIN_PWR_EN));
  pinMode(PIN_PWR_EN, OUTPUT);
  digitalWrite(PIN_PWR_EN, LOW);
}

void stopEspNow() {
  if (espNowInitialized) {
    esp_now_deinit();
    espNowInitialized = false;
  }
}

void stopRadio() {
  stopNetworkServices();
  stopEspNow();

  if (wifiRadioStarted || WiFi.getMode() != WIFI_OFF) {
    WiFi.disconnect(true, false);
    WiFi.mode(WIFI_OFF);
    esp_wifi_stop();
    wifiRadioStarted = false;
  }
  wifiAttemptActive = false;
}

String makeCommandUid() {
  ++commandSequence;

  char output[18];
  snprintf(output, sizeof(output), "%08lX-%04X-%04X",
           static_cast<unsigned long>(millis()),
           static_cast<unsigned int>(commandSequence),
           static_cast<unsigned int>(esp_random() & 0xFFFF));
  return String(output);
}

uint8_t percentFromMillivolts(uint16_t millivolts) {
  if (millivolts <= BATTERY_CURVE[0].millivolts) {
    return BATTERY_CURVE[0].percent;
  }

  const size_t lastIndex = sizeof(BATTERY_CURVE) / sizeof(BATTERY_CURVE[0]) - 1;
  if (millivolts >= BATTERY_CURVE[lastIndex].millivolts) {
    return BATTERY_CURVE[lastIndex].percent;
  }

  for (size_t i = 1; i <= lastIndex; ++i) {
    const BatteryCurvePoint low = BATTERY_CURVE[i - 1];
    const BatteryCurvePoint high = BATTERY_CURVE[i];
    if (millivolts <= high.millivolts) {
      const uint16_t mvRange = high.millivolts - low.millivolts;
      const uint8_t pctRange = high.percent - low.percent;
      const uint16_t mvOffset = millivolts - low.millivolts;
      return low.percent + (static_cast<uint32_t>(mvOffset) * pctRange) /
                               mvRange;
    }
  }

  return 0;
}

BatteryReading readBattery() {
  digitalWrite(PIN_BAT_EN, HIGH);
  delayMicroseconds(2500);

  uint32_t pinMillivolts = 0;
  constexpr uint8_t sampleCount = 8;
  for (uint8_t i = 0; i < sampleCount; ++i) {
    pinMillivolts += analogReadMilliVolts(PIN_BAT_VAL);
    delayMicroseconds(500);
  }

  digitalWrite(PIN_BAT_EN, LOW);

  const float averagePinMv = static_cast<float>(pinMillivolts) / sampleCount;
  BatteryReading reading;
  reading.millivolts =
      static_cast<uint16_t>(averagePinMv * BATTERY_DIVIDER_RATIO + 0.5f);
  reading.percent = percentFromMillivolts(reading.millivolts);
  return reading;
}

void refreshBattery(bool force = false) {
  const uint32_t now = millis();
  if (force || elapsed(now, lastBatteryRefreshMs, BATTERY_REFRESH_MS)) {
    battery = readBattery();
    lastBatteryRefreshMs = now;
  }
}


uint8_t batteryBarsFromPercent(uint8_t percent) {
  if (percent == 0) {
    return 0;
  }
  return min<uint8_t>(5, (percent + 19) / 20);
}

uint8_t visibleHeaderBatteryBars() {
  const uint8_t baseBars = batteryBarsFromPercent(battery.percent);
  if (!usbPresent || baseBars >= 5) {
    return baseBars;
  }
  return baseBars + (batteryChargeFrame % (6 - baseBars));
}

void drawHeaderBatteryIcon(int x, int y) {
  const uint8_t bars = visibleHeaderBatteryBars();
  u8g2.drawFrame(x, y, 20, 9);
  u8g2.drawBox(x + 20, y + 3, 2, 3);

  for (uint8_t i = 0; i < bars; ++i) {
    u8g2.drawBox(x + 3 + i * 3, y + 2, 2, 5);
  }
}

void drawHeader(const char *title) {
  u8g2.setDrawColor(1);
  u8g2.drawBox(0, 0, 128, 13);
  u8g2.setDrawColor(0);
  u8g2.setFont(u8g2_font_6x10_tf);
  u8g2.drawStr(3, 10, title);
  drawHeaderBatteryIcon(103, 2);
  u8g2.setDrawColor(1);
}

String fitTextToWidth(String text, int maxWidth) {
  text.trim();
  if (text.isEmpty() || u8g2.getStrWidth(text.c_str()) <= maxWidth) {
    return text;
  }

  while (text.length() > 0) {
    text.remove(text.length() - 1);
    String candidate = text + "...";
    if (u8g2.getStrWidth(candidate.c_str()) <= maxWidth) {
      return candidate;
    }
  }

  return "";
}

void drawCenteredString(int y, const String &text, int maxWidth = 124) {
  const String fitted = fitTextToWidth(text, maxWidth);
  const int width = u8g2.getStrWidth(fitted.c_str());
  const int x = max<int>(0, (128 - width) / 2);
  u8g2.drawStr(x, y, fitted.c_str());
}

void drawFooter(const String &left, const String &right) {
  u8g2.setFont(u8g2_font_6x10_tf);
  u8g2.drawHLine(0, 53, 128);
  u8g2.drawStr(0, 63, fitTextToWidth(left, 74).c_str());
  const String fittedRight = fitTextToWidth(right, 52);
  const int rightWidth = u8g2.getStrWidth(fittedRight.c_str());
  u8g2.drawStr(127 - rightWidth, 63, fittedRight.c_str());
}

void drawChargeScreen() {
  u8g2.clearBuffer();
  drawHeader("USB");

  const bool statusActive = otaInProgress ||
                            (!usbStatusLine.isEmpty() &&
                             static_cast<int32_t>(usbStatusUntilMs - millis()) >
                                 0);
  String networkLine;
  if (!hasCompleteWifiConfig()) {
    networkLine = "Wifi disabled";
  } else if (WiFi.status() == WL_CONNECTED) {
    const String ip = WiFi.localIP().toString();
    networkLine = "IP:" + ip + " CH:" + String(config.channel);
    u8g2.setFont(u8g2_font_6x10_tf);
    if (u8g2.getStrWidth(networkLine.c_str()) > 126) {
      networkLine = "IP " + ip + " C" + String(config.channel);
    }
    if (u8g2.getStrWidth(networkLine.c_str()) > 126) {
      networkLine = ip + " C" + String(config.channel);
    }
  } else if (wifiAttemptActive) {
    networkLine = "Wifi connecting";
  } else {
    networkLine = "Wifi offline";
  }
  if (WiFi.status() != WL_CONNECTED) {
    networkLine += " CH:" + String(config.channel);
  }

  u8g2.setFont(u8g2_font_6x10_tf);
  u8g2.drawStr(1, 24, fitTextToWidth(networkLine, 126).c_str());
  const String idLine =
      statusActive ? "Status:" + usbStatusLine : "ID:" + config.name;
  u8g2.drawStr(1, 36, fitTextToWidth(idLine, 126).c_str());
  u8g2.drawStr(1, 48,
               fitTextToWidth("CMD:" + config.cmd, 126).c_str());
  u8g2.drawStr(1, 60,
               fitTextToWidth("Target:" + config.to, 126).c_str());
  u8g2.sendBuffer();
}

void drawSendScreen(const String &line1, const String &line2) {
  u8g2.clearBuffer();
  drawHeader("REMOTE");
  u8g2.setFont(u8g2_font_7x13_tf);
  drawCenteredString(30, line1);
  u8g2.setFont(u8g2_font_6x10_tf);
  drawCenteredString(45, line2);
  drawFooter(config.name, String(battery.millivolts) + "mV");
  u8g2.sendBuffer();
}

void drawChannelSettingsScreen(uint8_t channel, const String &status) {
  u8g2.clearBuffer();
  drawHeader("SET CH");
  u8g2.setFont(u8g2_font_7x13B_tf);
  drawCenteredString(31, channel == AUTO_CHANNEL
                             ? String("AUTO")
                             : String("CH ") + String(channel));
  u8g2.setFont(u8g2_font_6x10_tf);
  drawCenteredString(45, status);
  drawFooter("Press Next", "Hold Save");
  u8g2.sendBuffer();
}

void drawWifiScanScreen(const String &status, const String &detail) {
  u8g2.clearBuffer();
  drawHeader("AUTO CH");
  u8g2.setFont(u8g2_font_7x13B_tf);
  drawCenteredString(31, status);
  u8g2.setFont(u8g2_font_6x10_tf);
  drawCenteredString(45, detail);
  drawFooter(config.name, "WiFi scan");
  u8g2.sendBuffer();
}

String serializeConfig(const DeviceConfig &value) {
  StaticJsonDocument<768> doc;
  JsonObject wifi = doc.createNestedObject("wifi");
  wifi["ssid"] = value.wifi.ssid;
  wifi["password"] = value.wifi.password;
  doc["id"] = value.id;
  doc["name"] = value.name;
  doc["to"] = value.to;
  doc["cmd"] = value.cmd;
  doc["data"] = serialized(value.dataJson);
  doc["channel"] = value.channel;
  doc["backlightBrightness"] = value.backlightBrightness;
  String output;
  serializeJsonPretty(doc, output);
  output += '\n';
  return output;
}

bool writeConfigAtomically(const String &json, String &error) {
  LittleFS.remove(CONFIG_TEMP_PATH);
  File temp = LittleFS.open(CONFIG_TEMP_PATH, "w");
  if (!temp) {
    error = "Unable to create temporary config file";
    return false;
  }

  const size_t written = temp.print(json);
  if (!json.endsWith("\n")) {
    temp.println();
  }
  temp.close();
  if (written != json.length()) {
    LittleFS.remove(CONFIG_TEMP_PATH);
    error = "Incomplete config write";
    return false;
  }

  LittleFS.remove(CONFIG_BACKUP_PATH);
  const bool hadConfig = LittleFS.exists(CONFIG_PATH);
  if (hadConfig && !LittleFS.rename(CONFIG_PATH, CONFIG_BACKUP_PATH)) {
    LittleFS.remove(CONFIG_TEMP_PATH);
    error = "Unable to back up config.json";
    return false;
  }
  if (!LittleFS.rename(CONFIG_TEMP_PATH, CONFIG_PATH)) {
    if (hadConfig) {
      LittleFS.rename(CONFIG_BACKUP_PATH, CONFIG_PATH);
    }
    LittleFS.remove(CONFIG_TEMP_PATH);
    error = "Unable to replace config.json";
    return false;
  }

  LittleFS.remove(CONFIG_BACKUP_PATH);
  error = "";
  return true;
}

void saveDefaultConfig() {
  String error;
  writeConfigAtomically(serializeConfig(DeviceConfig()), error);
}

bool saveConfig() {
  if (serialConfig.restartRequired()) return false;
  String error;
  const bool saved = writeConfigAtomically(serializeConfig(config), error);
  if (!saved) {
    Serial.println("Config save failed: " + error);
  }
  return saved;
}

String readRawConfig(String &error) {
  error = "";
  File file = LittleFS.open(CONFIG_PATH, "r");
  if (!file) {
    error = "Unable to open config.json";
    return "";
  }
  if (file.size() > MAX_CONFIG_FILE_LEN) {
    file.close();
    error = "config.json exceeds 4096 bytes";
    return "";
  }
  String raw = file.readString();
  file.close();
  return raw;
}

bool requireConfigString(JsonVariantConst value, const char *field,
                         size_t maxLength, bool allowEmpty, String &output,
                         String &error) {
  if (!value.is<const char *>()) {
    error = String(field) + " must be a string";
    return false;
  }
  output = value.as<const char *>();
  if (output.length() > maxLength) {
    error = String(field) + " must not exceed " + String(maxLength) +
            " bytes";
    return false;
  }
  String trimmed = output;
  trimmed.trim();
  if (!allowEmpty && trimmed.isEmpty()) {
    error = String(field) + " must not be empty";
    return false;
  }
  return true;
}

bool parseAndValidateConfig(const String &json, DeviceConfig &parsed,
                            String &error) {
  if (json.isEmpty()) {
    error = "config.json is empty";
    return false;
  }
  if (json.length() > MAX_CONFIG_FILE_LEN) {
    error = "config.json exceeds 4096 bytes";
    return false;
  }

  DynamicJsonDocument doc(2048);
  const DeserializationError jsonError = deserializeJson(doc, json);
  if (jsonError) {
    error = "Invalid JSON: " + String(jsonError.c_str());
    return false;
  }
  if (!doc.is<JsonObject>()) {
    error = "The JSON root must be an object";
    return false;
  }

  JsonObjectConst root = doc.as<JsonObjectConst>();
  if (!root["wifi"].is<JsonObjectConst>()) {
    error = "wifi must be an object";
    return false;
  }
  JsonObjectConst wifi = root["wifi"].as<JsonObjectConst>();
  DeviceConfig candidate;
  if (!requireConfigString(wifi["ssid"], "wifi.ssid", MAX_WIFI_SSID_LEN,
                           true, candidate.wifi.ssid, error) ||
      !requireConfigString(wifi["password"], "wifi.password",
                           MAX_WIFI_PASSWORD_LEN, true,
                           candidate.wifi.password, error) ||
      !requireConfigString(root["id"], "id", MAX_ID_LEN, false,
                           candidate.id, error) ||
      !requireConfigString(root["name"], "name", MAX_NAME_LEN, false,
                           candidate.name, error) ||
      !requireConfigString(root["to"], "to", MAX_TO_LEN, false,
                           candidate.to, error) ||
      !requireConfigString(root["cmd"], "cmd", MAX_CMD_LEN, false,
                           candidate.cmd, error)) {
    return false;
  }

  if (!root["data"].is<JsonObjectConst>()) {
    error = "data must be an object";
    return false;
  }
  candidate.dataJson = "";
  serializeJson(root["data"], candidate.dataJson);
  if (candidate.dataJson.length() > MAX_DATA_JSON_LEN) {
    error = "Serialized data must not exceed 96 bytes";
    return false;
  }

  if (!root["channel"].is<int>()) {
    error = "channel must be an integer";
    return false;
  }
  const int channel = root["channel"].as<int>();
  if (channel < MIN_CHANNEL || channel > MAX_CHANNEL) {
    error = "channel must be 1-13";
    return false;
  }
  candidate.channel = static_cast<uint8_t>(channel);

  if (!root["backlightBrightness"].is<int>()) {
    error = "backlightBrightness must be an integer";
    return false;
  }
  const int brightness = root["backlightBrightness"].as<int>();
  if (brightness < 0 || brightness > 100) {
    error = "backlightBrightness must be 0-100";
    return false;
  }
  candidate.backlightBrightness = static_cast<uint8_t>(brightness);

  StaticJsonDocument<384> packet;
  packet["id"] = candidate.id;
  packet["uid"] = "FFFFFFFF-FFFF-FFFF";
  packet["to"] = candidate.to;
  packet["cmd"] = candidate.cmd;
  packet["dat"] = serialized(candidate.dataJson);
  packet["bat"] = 100;
  packet["chk"] = "FFFFFFFF";
  if (measureJson(packet) >= ESPNOW_PAYLOAD_MAX_LEN) {
    error = "id/to/cmd/data make the ESP-NOW packet exceed 249 bytes";
    return false;
  }

  parsed = candidate;
  error = "";
  return true;
}

void loadConfig() {
  if (!LittleFS.exists(CONFIG_PATH)) {
    saveDefaultConfig();
  }

  bool shouldRewrite = false;
  File file = LittleFS.open(CONFIG_PATH, "r");
  if (!file) {
    config = DeviceConfig();
    saveDefaultConfig();
    return;
  }

  StaticJsonDocument<768> doc;
  DeserializationError err = deserializeJson(doc, file);
  file.close();

  if (err) {
    config = DeviceConfig();
    saveDefaultConfig();
    return;
  }

  JsonObjectConst wifi = doc["wifi"].as<JsonObjectConst>();
  config.wifi.ssid =
      configStringOrEmpty(wifi["ssid"], MAX_WIFI_SSID_LEN, shouldRewrite);
  config.wifi.password = configStringOrEmpty(
      wifi["password"], MAX_WIFI_PASSWORD_LEN, shouldRewrite);

  config.id = limitString(doc["id"] | DEFAULT_ID, MAX_ID_LEN, DEFAULT_ID);
  config.name =
      limitString(doc["name"] | DEFAULT_NAME, MAX_NAME_LEN, DEFAULT_NAME);
  config.to = limitString(doc["to"] | DEFAULT_TO, MAX_TO_LEN, DEFAULT_TO);
  config.cmd = limitString(doc["cmd"] | DEFAULT_CMD, MAX_CMD_LEN, DEFAULT_CMD);
  config.dataJson =
      compactJsonOrDefault(doc["data"], DEFAULT_DATA_JSON, MAX_DATA_JSON_LEN,
                           shouldRewrite);

  int channel = doc["channel"] | DEFAULT_CHANNEL;
  if (channel < MIN_CHANNEL || channel > MAX_CHANNEL) {
    channel = DEFAULT_CHANNEL;
    shouldRewrite = true;
  }
  config.channel = static_cast<uint8_t>(channel);

  config.backlightBrightness =
      clampPercent(doc["backlightBrightness"] | DEFAULT_BACKLIGHT_BRIGHTNESS,
                   DEFAULT_BACKLIGHT_BRIGHTNESS, shouldRewrite);

  if (config.id != String(doc["id"] | "") ||
      config.name != String(doc["name"] | "") ||
      config.to != String(doc["to"] | "") ||
      config.cmd != String(doc["cmd"] | "") ||
      !doc.containsKey("data") || !doc.containsKey("channel") ||
      !doc.containsKey("cmd") || !doc.containsKey("backlightBrightness")) {
    shouldRewrite = true;
  }

  if (shouldRewrite) {
    saveConfig();
  }
}

bool hasCompleteWifiConfig() {
  String ssid = config.wifi.ssid;
  String password = config.wifi.password;
  ssid.trim();
  password.trim();
  return !ssid.isEmpty() && !password.isEmpty();
}

void sendWebJsonError(int statusCode, const String &message) {
  DynamicJsonDocument doc(256);
  doc["ok"] = false;
  doc["error"] = message;
  String response;
  serializeJson(doc, response);
  webServer.send(statusCode, "application/json", response);
}

void handleWebStatus() {
  DynamicJsonDocument doc(1024);
  doc["name"] = config.name;
  doc["id"] = config.id;
  doc["target"] = config.to;
  doc["command"] = config.cmd;
  doc["espnowChannel"] = config.channel;
  doc["ip"] = WiFi.localIP().toString();
  doc["mac"] = WiFi.macAddress();
  doc["ssid"] = WiFi.SSID();
  doc["rssi"] = WiFi.RSSI();
  doc["wifiChannel"] = WiFi.channel();
  doc["mdns"] = NETWORK_HOSTNAME;
  doc["uptime"] = millis() / 1000UL;
  doc["freeHeap"] = ESP.getFreeHeap();
  doc["fsUsed"] = LittleFS.usedBytes();
  doc["fsTotal"] = LittleFS.totalBytes();
  doc["batteryMillivolts"] = battery.millivolts;
  doc["batteryPercent"] = battery.percent;
  doc["usbPresent"] = usbPresent;
  doc["otaActive"] = otaInProgress;
  String response;
  serializeJson(doc, response);
  webServer.sendHeader("Cache-Control", "no-store");
  webServer.send(200, "application/json", response);
}

void handleWebConfigGet() {
  String error;
  const String raw = readRawConfig(error);
  if (!error.isEmpty()) {
    sendWebJsonError(500, error);
    return;
  }
  webServer.sendHeader("Cache-Control", "no-store");
  webServer.send(200, "application/json; charset=utf-8", raw);
}

void handleWebConfigSave() {
  DeviceConfig updated;
  String error;
  const String body = webServer.arg("plain");
  if (!parseAndValidateConfig(body, updated, error)) {
    sendWebJsonError(400, error);
    return;
  }

  const bool wifiChanged = updated.wifi.ssid != config.wifi.ssid ||
                           updated.wifi.password != config.wifi.password;
  if (!writeConfigAtomically(body, error)) {
    sendWebJsonError(500, error);
    return;
  }

  config = updated;
  setBacklight(backlightOn);
  if (mdnsStarted) {
    MDNS.setInstanceName(config.name);
  }
  DynamicJsonDocument responseDoc(192);
  responseDoc["ok"] = true;
  responseDoc["saved"] = true;
  responseDoc["wifiChanged"] = wifiChanged;
  String response;
  serializeJson(responseDoc, response);
  webServer.send(200, "application/json", response);

  if (wifiChanged) {
    wifiRestartPending = true;
    wifiRestartAtMs = millis() + WIFI_RESTART_DELAY_MS;
  }
}

void configureWebRoutes() {
  if (webRoutesConfigured) {
    return;
  }
  webServer.on("/", HTTP_GET, []() {
    webServer.sendHeader("Cache-Control", "no-store");
    webServer.send_P(200, "text/html; charset=utf-8", WEB_INDEX_HTML);
  });
  webServer.on("/api/status", HTTP_GET, handleWebStatus);
  webServer.on("/api/config", HTTP_GET, handleWebConfigGet);
  webServer.on("/api/config", HTTP_PUT, handleWebConfigSave);
  webServer.on("/api/config", HTTP_POST, handleWebConfigSave);
  webServer.on("/api/reboot", HTTP_POST, []() {
    webServer.send(200, "application/json",
                   "{\"ok\":true,\"restarting\":true}");
    rebootPending = true;
    rebootAtMs = millis() + REBOOT_DELAY_MS;
  });
  webServer.onNotFound([]() { sendWebJsonError(404, "Not found"); });
  webRoutesConfigured = true;
}

void configureArduinoOta() {
  if (arduinoOtaConfigured) {
    return;
  }
  ArduinoOTA.setHostname(NETWORK_HOSTNAME);
  ArduinoOTA.setPort(3232);
  ArduinoOTA.setPartitionLabel("littlefs");
  ArduinoOTA.setRebootOnSuccess(true);
  ArduinoOTA.setMdnsEnabled(false);
  ArduinoOTA.onStart([]() {
    otaInProgress = true;
    otaLastProgress = UINT8_MAX;
    setBacklight(true);
    usbStatusLine = "OTA updating";
    usbStatusUntilMs = UINT32_MAX;
    drawChargeScreen();
    Serial.println("ESPOTA update started");
  });
  ArduinoOTA.onProgress([](unsigned int progress, unsigned int total) {
    const uint8_t percent = total == 0
                                ? 0
                                : static_cast<uint8_t>(
                                      (progress * 100ULL) / total);
    if (otaLastProgress == UINT8_MAX || percent >= otaLastProgress + 10 ||
        percent == 100) {
      otaLastProgress = percent;
      usbStatusLine = "OTA " + String(percent) + "%";
      drawChargeScreen();
      Serial.printf("ESPOTA progress: %u%%\n", percent);
    }
  });
  ArduinoOTA.onEnd([]() {
    usbStatusLine = "OTA complete";
    usbStatusUntilMs = millis() + USB_STATUS_VISIBLE_MS;
    drawChargeScreen();
    Serial.println("ESPOTA update complete");
  });
  ArduinoOTA.onError([](ota_error_t error) {
    otaInProgress = false;
    usbStatusLine = "OTA error " + String(static_cast<unsigned>(error));
    usbStatusUntilMs = millis() + USB_STATUS_VISIBLE_MS;
    drawChargeScreen();
    Serial.printf("ESPOTA error: %u\n", static_cast<unsigned>(error));
  });
  arduinoOtaConfigured = true;
}

void startNetworkServices() {
  if (!usbPresent || !hasCompleteWifiConfig() ||
      WiFi.status() != WL_CONNECTED) {
    return;
  }

  configureWebRoutes();
  configureArduinoOta();
  if (!mdnsStarted) {
    mdnsStarted = MDNS.begin(NETWORK_HOSTNAME);
    if (mdnsStarted) {
      MDNS.setInstanceName(config.name);
      MDNS.addService("http", "tcp", 80);
      MDNS.enableArduino(3232, false);
    } else {
      Serial.println("mDNS start failed");
    }
  }
  if (!webServerStarted) {
    webServer.begin();
    webServerStarted = true;
  }
  if (!arduinoOtaStarted) {
    ArduinoOTA.begin();
    arduinoOtaStarted = true;
  }
}

void stopNetworkServices() {
  if (arduinoOtaStarted) {
    ArduinoOTA.end();
    arduinoOtaStarted = false;
  }
  if (webServerStarted) {
    webServer.stop();
    webServerStarted = false;
  }
  if (mdnsStarted) {
    MDNS.end();
    mdnsStarted = false;
  }
  otaInProgress = false;
}

void startWifiAttempt() {
  if (!usbPresent || !readUsbPresent() || !hasCompleteWifiConfig()) {
    stopRadio();
    nextWifiAttemptMs = millis() + WIFI_RETRY_MS;
    return;
  }

  stopRadio();
  WiFi.persistent(false);
  if (!WiFi.mode(WIFI_STA)) {
    nextWifiAttemptMs = millis() + WIFI_RETRY_MS;
    return;
  }
  wifiRadioStarted = true;
  WiFi.setHostname(NETWORK_HOSTNAME);
  WiFi.setSleep(true);
  WiFi.setAutoReconnect(false);
  WiFi.begin(config.wifi.ssid.c_str(), config.wifi.password.c_str());
  wifiAttemptActive = true;
  wifiAttemptStartedMs = millis();
  nextChannelSyncAttemptMs = wifiAttemptStartedMs;
  Serial.println("Connecting Wi-Fi: " + config.wifi.ssid);
}

void syncConnectedWifiChannel(uint32_t now) {
  if (serialConfig.restartRequired()) return;
  const int actualChannel = WiFi.channel();
  if (actualChannel < MIN_CHANNEL || actualChannel > MAX_CHANNEL ||
      actualChannel == config.channel ||
      static_cast<int32_t>(now - nextChannelSyncAttemptMs) < 0) {
    return;
  }

  nextChannelSyncAttemptMs = now + WIFI_RETRY_MS;
  const uint8_t previousChannel = config.channel;
  config.channel = static_cast<uint8_t>(actualChannel);
  if (saveConfig()) {
    Serial.printf("Wi-Fi channel synchronized: CH%u -> CH%d\n",
                  previousChannel, actualChannel);
    drawChargeScreen();
  } else {
    config.channel = previousChannel;
  }
}

void serviceNetwork() {
  const uint32_t now = millis();
  if (rebootPending && static_cast<int32_t>(now - rebootAtMs) >= 0) {
    ESP.restart();
  }

  if (!usbPresent || !readUsbPresent() || !hasCompleteWifiConfig()) {
    if (wifiRadioStarted || webServerStarted || arduinoOtaStarted ||
        mdnsStarted) {
      stopRadio();
    }
    return;
  }

  if (wifiRestartPending &&
      static_cast<int32_t>(now - wifiRestartAtMs) >= 0) {
    wifiRestartPending = false;
    startWifiAttempt();
    return;
  }

  if (WiFi.status() == WL_CONNECTED) {
    wifiAttemptActive = false;
    syncConnectedWifiChannel(now);
    startNetworkServices();
    webServer.handleClient();
    if (arduinoOtaStarted) {
      ArduinoOTA.handle();
    }
    return;
  }

  stopNetworkServices();
  if (wifiAttemptActive &&
      elapsed(now, wifiAttemptStartedMs, WIFI_CONNECT_TIMEOUT_MS)) {
    WiFi.disconnect(false, false);
    wifiAttemptActive = false;
    nextWifiAttemptMs = now + WIFI_RETRY_MS;
    Serial.println("Wi-Fi connection timed out; retry in 60 seconds");
    return;
  }

  if (!wifiAttemptActive &&
      static_cast<int32_t>(now - nextWifiAttemptMs) >= 0) {
    startWifiAttempt();
  }
}

#if ESP_ARDUINO_VERSION_MAJOR >= 3
void onEspNowSent(const wifi_tx_info_t *, esp_now_send_status_t status) {
#else
void onEspNowSent(const uint8_t *, esp_now_send_status_t status) {
#endif
  espNowLastStatus = status;
  espNowSendComplete = true;
}

SendResult sendEspNowCommand() {
  const bool preserveConnectedWifi =
      usbPresent && readUsbPresent() && hasCompleteWifiConfig() &&
      WiFi.status() == WL_CONNECTED;
  stopEspNow();

  if (!preserveConnectedWifi) {
    stopRadio();
    if (!WiFi.mode(WIFI_STA)) {
      return SendResult::Error;
    }
    wifiRadioStarted = true;
    WiFi.disconnect();
    yield();

    esp_wifi_set_promiscuous(true);
    const esp_err_t channelErr =
        esp_wifi_set_channel(config.channel, WIFI_SECOND_CHAN_NONE);
    esp_wifi_set_promiscuous(false);
    if (channelErr != ESP_OK) {
      stopRadio();
      return SendResult::Error;
    }
  }

  const auto finishRadioUse = [preserveConnectedWifi]() {
    stopEspNow();
    if (!preserveConnectedWifi) {
      stopRadio();
    }
  };

  if (esp_now_init() != ESP_OK) {
    finishRadioUse();
    return SendResult::Error;
  }
  espNowInitialized = true;

  esp_now_register_send_cb(onEspNowSent);

  esp_now_peer_info_t peerInfo = {};
  memcpy(peerInfo.peer_addr, ESPNOW_BROADCAST_MAC, sizeof(ESPNOW_BROADCAST_MAC));
  peerInfo.channel = preserveConnectedWifi ? 0 : config.channel;
  peerInfo.encrypt = false;
#ifdef WIFI_IF_STA
  peerInfo.ifidx = WIFI_IF_STA;
#endif

  if (!esp_now_is_peer_exist(ESPNOW_BROADCAST_MAC)) {
    if (esp_now_add_peer(&peerInfo) != ESP_OK) {
      finishRadioUse();
      return SendResult::Error;
    }
  }

  StaticJsonDocument<384> doc;
  const String uid = makeCommandUid();
  doc["id"] = config.id;
  doc["uid"] = uid;
  doc["to"] = config.to;
  doc["cmd"] = config.cmd;
  doc["dat"] = serialized(config.dataJson);
  doc["bat"] = battery.percent;
  doc["chk"] = commandChecksum(doc);

  char payload[ESPNOW_PAYLOAD_MAX_LEN];
  const size_t payloadSize = serializeJson(doc, payload, sizeof(payload));
  if (payloadSize == 0 || payloadSize >= sizeof(payload)) {
    finishRadioUse();
    return SendResult::Error;
  }

  bool anySuccess = false;
  bool anyTimeout = false;
  for (uint8_t attempt = 0; attempt < COMMAND_SEND_ATTEMPTS; ++attempt) {
    espNowSendComplete = false;
    espNowLastStatus = ESP_NOW_SEND_FAIL;

    const esp_err_t sendErr =
        esp_now_send(ESPNOW_BROADCAST_MAC, reinterpret_cast<uint8_t *>(payload),
                     payloadSize);
    if (sendErr == ESP_OK) {
      const uint32_t started = millis();
      while (!espNowSendComplete &&
             !elapsed(millis(), started, ESPNOW_SEND_WAIT_MS)) {
        yield();
      }

      if (!espNowSendComplete) {
        anyTimeout = true;
      } else if (espNowLastStatus == ESP_NOW_SEND_SUCCESS) {
        anySuccess = true;
      }
    }

    if (attempt + 1 < COMMAND_SEND_ATTEMPTS) {
      delay(ESPNOW_SEND_GAP_MS);
    }
  }

  finishRadioUse();

  if (anySuccess) {
    return SendResult::Ok;
  }
  if (anyTimeout) {
    return SendResult::Timeout;
  }
  return SendResult::Error;
}

const char *sendResultText(SendResult result) {
  switch (result) {
  case SendResult::Ok:
    return "Sent OK";
  case SendResult::Timeout:
    return "Send timeout";
  case SendResult::Error:
  default:
    return "Send failed";
  }
}

SendResult sendTrigger(bool showSendScreen = true) {
  refreshBattery(true);
  if (showSendScreen) {
    drawSendScreen("Sending...", config.name);
  }
  const SendResult result = sendEspNowCommand();
  refreshBattery(true);
  if (showSendScreen) {
    drawSendScreen(sendResultText(result), String("CH ") + String(config.channel));
  }
  return result;
}

void shutdownNow() {
  if (readUsbPresent()) {
    return;
  }

  setBacklight(false);
  digitalWrite(PIN_BAT_EN, LOW);
  stopRadio();
  holdPowerOff();

  delay(80);
  esp_sleep_disable_wakeup_source(ESP_SLEEP_WAKEUP_ALL);
  esp_deep_sleep_start();
}

void enterPowerOffAfterResult() {
  const uint32_t shownAtMs = millis();
  while (!readUsbPresent() && !elapsed(millis(), shownAtMs, RESULT_VISIBLE_MS)) {
    delay(10);
  }
  if (readUsbPresent()) {
    usbPresent = true;
    lastUsbActivityMs = millis();
    setBacklight(true);
    refreshBattery(true);
    drawChargeScreen();
    return;
  }

  setBacklight(false);
  const uint32_t dimmedAtMs = millis();
  while (!readUsbPresent() &&
         !elapsed(millis(), dimmedAtMs, POST_BACKLIGHT_OFF_MS)) {
    delay(10);
  }
  if (readUsbPresent()) {
    usbPresent = true;
    lastUsbActivityMs = millis();
    setBacklight(true);
    refreshBattery(true);
    drawChargeScreen();
    return;
  }

  shutdownNow();
}

ButtonEvent pollButtonEvent() {
  const bool raw = readButtonPressed();
  const uint32_t now = millis();

  if (raw != lastButtonRaw) {
    lastButtonRaw = raw;
    buttonChangedAtMs = now;
  }

  if (elapsed(now, buttonChangedAtMs, BUTTON_DEBOUNCE_MS) &&
      raw != stableButtonPressed) {
    stableButtonPressed = raw;
    return stableButtonPressed ? ButtonEvent::Pressed : ButtonEvent::Released;
  }

  return ButtonEvent::None;
}

uint8_t nextChannel(uint8_t channel) {
  if (channel == AUTO_CHANNEL) {
    return MIN_CHANNEL;
  }
  if (channel >= MAX_CHANNEL) {
    return config.wifi.ssid.isEmpty() ? MIN_CHANNEL : AUTO_CHANNEL;
  }
  return channel + 1;
}

WifiChannelScanResult scanConfiguredWifiChannel() {
  WifiChannelScanResult result;
  if (config.wifi.ssid.isEmpty()) {
    result.status = WifiChannelScanStatus::NotFound;
    return result;
  }

  stopRadio();
  wifiRadioStarted = true;
  if (!WiFi.mode(WIFI_STA)) {
    stopRadio();
    return result;
  }
  WiFi.disconnect();
  yield();

  const int16_t networkCount = WiFi.scanNetworks(false, true);
  if (networkCount < 0) {
    WiFi.scanDelete();
    stopRadio();
    return result;
  }

  int32_t strongestRssi = -1000;
  for (int16_t i = 0; i < networkCount && i <= UINT8_MAX; ++i) {
    const uint8_t networkIndex = static_cast<uint8_t>(i);
    if (WiFi.SSID(networkIndex) != config.wifi.ssid) {
      continue;
    }

    const int32_t channel = WiFi.channel(networkIndex);
    const int32_t rssi = WiFi.RSSI(networkIndex);
    if (channel >= MIN_CHANNEL && channel <= MAX_CHANNEL &&
        (result.channel == AUTO_CHANNEL || rssi > strongestRssi)) {
      result.channel = static_cast<uint8_t>(channel);
      strongestRssi = rssi;
    }
  }

  WiFi.scanDelete();
  stopRadio();
  result.status = result.channel == AUTO_CHANNEL
                      ? WifiChannelScanStatus::NotFound
                      : WifiChannelScanStatus::Found;
  return result;
}

void waitForButtonRelease() {
  while (readButtonPressed()) {
    pollButtonEvent();
    delay(10);
  }
  setupButtonState();
}

bool enterChannelSettingsMode() {
  uint8_t selectedChannel = config.channel;
  uint32_t lastActivityMs = millis();
  uint32_t settingsPressStartedMs = 0;
  bool settingsLongHandled = false;

  setBacklight(true);
  drawChannelSettingsScreen(selectedChannel, "Short: next");
  waitForButtonRelease();
  lastActivityMs = millis();

  while (true) {
    const uint32_t now = millis();
    if (usbPresent) {
      serviceNetwork();
    }
    const ButtonEvent event = pollButtonEvent();

    if (event == ButtonEvent::Pressed) {
      settingsPressStartedMs = now;
      settingsLongHandled = false;
      lastActivityMs = now;
    }

    if (stableButtonPressed && !settingsLongHandled &&
        elapsed(now, settingsPressStartedMs, BUTTON_LONG_PRESS_MS)) {
      settingsLongHandled = true;

      if (selectedChannel == AUTO_CHANNEL) {
        drawWifiScanScreen("Scanning...", config.wifi.ssid);
        const WifiChannelScanResult scanResult = scanConfiguredWifiChannel();
        if (scanResult.status == WifiChannelScanStatus::Found) {
          config.channel = scanResult.channel;
          const bool saved = saveConfig();
          drawWifiScanScreen(String("Found CH ") + String(scanResult.channel),
                             saved ? "Saved" : "Save failed");
          delay(CHANNEL_SETTINGS_RESULT_MS);
          waitForButtonRelease();
          return saved;
        }

        drawWifiScanScreen(
            scanResult.status == WifiChannelScanStatus::NotFound
                ? String("SSID not found")
                : String("Scan failed"),
            config.wifi.ssid);
        delay(CHANNEL_SETTINGS_RESULT_MS);
        waitForButtonRelease();
        return false;
      }

      config.channel = selectedChannel;
      const bool saved = saveConfig();
      drawChannelSettingsScreen(selectedChannel, saved ? "Saved" : "Save failed");
      delay(CHANNEL_SETTINGS_RESULT_MS);
      waitForButtonRelease();
      return saved;
    }

    if (event == ButtonEvent::Released && !settingsLongHandled) {
      selectedChannel = nextChannel(selectedChannel);
      lastActivityMs = now;
      drawChannelSettingsScreen(selectedChannel, "Short: next");
    }

    if (!stableButtonPressed &&
        elapsed(now, lastActivityMs, CHANNEL_SETTINGS_TIMEOUT_MS)) {
      drawChannelSettingsScreen(config.channel, "Canceled");
      delay(CHANNEL_SETTINGS_RESULT_MS);
      setupButtonState();
      return false;
    }

    delay(10);
  }
}

void serviceChargeMode() {
  const uint32_t now = millis();

  if (!readUsbPresent()) {
    usbPresent = false;
    stopRadio();
    drawSendScreen("USB removed", "Powering off");
    delay(RESULT_VISIBLE_MS);
    setBacklight(false);
    delay(POST_BACKLIGHT_OFF_MS);
    shutdownNow();
    return;
  }

  serviceNetwork();
  if (otaInProgress) {
    delay(2);
    return;
  }

  const ButtonEvent buttonEvent = pollButtonEvent();
  if (buttonEvent == ButtonEvent::Pressed) {
    lastUsbActivityMs = millis();
    buttonPressStartedMs = millis();
    buttonLongHandled = false;
    setBacklight(true);
  }

  if (stableButtonPressed && !buttonLongHandled &&
      elapsed(now, buttonPressStartedMs, BUTTON_LONG_PRESS_MS)) {
    buttonLongHandled = true;
    usbStatusLine = "";
    enterChannelSettingsMode();
    lastUsbActivityMs = millis();
    lastUsbRefreshMs = millis();
    drawChargeScreen();
    return;
  }

  if (buttonEvent == ButtonEvent::Released && !buttonLongHandled) {
    lastUsbActivityMs = millis();
    setBacklight(true);
    usbStatusLine = "Sending...";
    usbStatusUntilMs = millis() + USB_STATUS_VISIBLE_MS;
    drawChargeScreen();

    SendResult result = sendTrigger(false);
    lastUsbActivityMs = millis();
    usbStatusLine = sendResultText(result);
    usbStatusUntilMs = millis() + USB_STATUS_VISIBLE_MS;
    lastUsbRefreshMs = millis();
    drawChargeScreen();
  }

  if (elapsed(now, lastUsbRefreshMs, USB_REFRESH_MS)) {
    lastUsbRefreshMs = now;
    ++batteryChargeFrame;
    refreshBattery();
    const bool statusActive = otaInProgress ||
                              (!usbStatusLine.isEmpty() &&
                               static_cast<int32_t>(usbStatusUntilMs - millis()) >
                                   0);
    if (!usbStatusLine.isEmpty() && !statusActive) {
      usbStatusLine = "";
    }
    if (backlightOn && !statusActive &&
        elapsed(now, lastUsbActivityMs, USB_BACKLIGHT_IDLE_MS)) {
      setBacklight(false);
    }
    drawChargeScreen();
  }
}

void handleRemoteStartup() {
  if (stableButtonPressed) {
    buttonPressStartedMs = millis();
    buttonLongHandled = false;
    drawSendScreen("Ready", String("CH ") + String(config.channel));

    while (stableButtonPressed) {
      const uint32_t now = millis();
      pollButtonEvent();
      if (stableButtonPressed && !buttonLongHandled &&
          elapsed(now, buttonPressStartedMs, BUTTON_LONG_PRESS_MS)) {
        buttonLongHandled = true;
        enterChannelSettingsMode();
        return;
      }
      delay(10);
    }
  }

  sendTrigger();
}

void setupPins() {
  lockPower();

  pinMode(PIN_LCD_BL, OUTPUT);
  pinMode(PIN_BAT_EN, OUTPUT);
  digitalWrite(PIN_BAT_EN, LOW);
  pinMode(PIN_USB_DET, INPUT);
  pinMode(PIN_BUTTON, INPUT);

  setupBacklightPwm();
  setBacklight(true);

  analogReadResolution(12);
  analogSetAttenuation(ADC_11db);
}

void setupDisplay() {
  u8g2.begin();
  u8g2.setContrast(20);
  u8g2.setDisplayRotation(U8G2_R2);
  u8g2.setPowerSave(0);
}

void setupStorage() {
  const bool mounted = LittleFS.begin(false, "/littlefs", 10, "littlefs");
  serialConfig.begin("RemoteBox", mounted,
      [](const String &content, String &error) {
        DeviceConfig candidate;
        return parseAndValidateConfig(content, candidate, error);
      }, [] { ESP.restart(); });
  if (!mounted) {
    config = DeviceConfig();
    return;
  }
  LittleFS.remove(CONFIG_TEMP_PATH);
  if (!LittleFS.exists(CONFIG_PATH) &&
      LittleFS.exists(CONFIG_BACKUP_PATH)) {
    LittleFS.rename(CONFIG_BACKUP_PATH, CONFIG_PATH);
  } else if (LittleFS.exists(CONFIG_BACKUP_PATH)) {
    LittleFS.remove(CONFIG_BACKUP_PATH);
  }
  loadConfig();
}

void setupButtonState() {
  lastButtonRaw = readButtonPressed();
  stableButtonPressed = lastButtonRaw;
  buttonChangedAtMs = millis();
}

} // namespace

void setup() {
  setupPins();
  Serial.setRxBufferSize(2048);
  Serial.begin(115200);
  delay(5);

  stopRadio();
  setupDisplay();
  setupStorage();
  setBacklight(true);
  setupButtonState();

  usbPresent = readUsbPresent();
  refreshBattery(true);

  Serial.printf("Starting up: %s\n", usbPresent ? "USB charge" : "send trigger");

  if (usbPresent) {
    if (hasCompleteWifiConfig()) {
      startWifiAttempt();
    }
    lastUsbActivityMs = millis();
    drawChargeScreen();
  } else {
    handleRemoteStartup();
    enterPowerOffAfterResult();
  }
}

void loop() {
  serialConfig.poll();
  if (usbPresent) {
    serviceChargeMode();
  }
  delay(1);
}
