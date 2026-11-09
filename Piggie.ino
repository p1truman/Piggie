/*
  Guinea Pig Hutch Controller - ESP32-S3
  Relay logic: ACTIVE HIGH (HIGH = ON)
  Heat pad relay (AUTO uses FLOOR DS18B20)
  Two camera relays: Play Cam and Bed Cam
  Ambient temperature + humidity: SHT40 (I2C)
  Updates: ArduinoOTA (IDE) and browser upload on the dashboard
  Web page is in index_html.h
*/

#include <WiFi.h>
#include <WebServer.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <Wire.h>
#include <OneWire.h>
#include <DallasTemperature.h>
#include <Preferences.h>
#include <ArduinoJson.h>
#include <ArduinoOTA.h>
#include <Update.h>
#include <esp_system.h>
#include <time.h>
#include <math.h>
#include <string.h>
#include "index_html.h"

// ================== USER CONFIG ==================
const char* WIFI_SSID = "";
const char* WIFI_PASS = "";

const char* BOT_TOKEN = "";
const char* CHAT_ID   = "";

const char* DASH_USER = "";
const char* DASH_PASS = "";

const char* OTA_PASS  = "";


// ---- Static IP (edit for your network) ----

const bool USE_STATIC_IP = true;

IPAddress STATIC_IP(192, 168, 2, 200);
IPAddress GATEWAY(192, 168, 2, 1);
IPAddress SUBNET(255, 255, 255, 0);
IPAddress DNS1(8, 8, 8, 8);
IPAddress DNS2(1, 1, 1, 1);

// ---- Pins ----

// Sensors on the right-hand header, next to the 3V3 pins
#define ONEWIRE_PIN    4   // Both DS18B20s; 4.7k pull-up to 3V3
#define I2C_SDA_PIN    5   // SHT40
#define I2C_SCL_PIN    6   // SHT40

// Relays on the left-hand header
#define RELAY_HEAT     1
#define RELAY_CAM_PLAY 2
#define RELAY_CAM_BED  42

const bool RELAY_ACTIVE_LOW = false;

// DS18B20 floor sensor address
DeviceAddress ADDR_FLOOR = {
  0x28, 0x24, 0xBE, 0xD3, 0x00, 0x00, 0x00, 0x10
};

// The shed sensor is discovered at startup.
DeviceAddress ADDR_SHED;
bool shedSensorFound = false;

// DS18B20 resolution:
// 9-bit ~94ms, 10-bit ~188ms, 11-bit ~375ms, 12-bit ~750ms
const uint8_t SENSOR_RESOLUTION_BITS = 10;

// SHT40
const uint8_t SHT_ADDR = 0x44;
const uint32_t I2C_HZ = 100000;

// Auto heating thresholds (FLOOR based)
float DEF_ON_BELOW  = 18.0;
float DEF_OFF_ABOVE = 21.0;

// Alert defaults (dashboard editable)
float DEF_FLOOR_COLD = 16.0;
float DEF_FLOOR_HOT  = 35.0;
float DEF_AMB_COLD   = 15.0;
float DEF_AMB_HOT    = 27.0;
float DEF_HUM_LOW    = 30.0;
float DEF_HUM_HIGH   = 75.0;

// 2h continuous heat runtime alert
const unsigned long HEAT_MAX_RUN_MS =
  2UL * 60UL * 60UL * 1000UL;

// Heat ON but floor not warming
const unsigned long NOHEAT_CHECK_MS =
  45UL * 60UL * 1000UL;

const float NOHEAT_MIN_RISE_C = 1.0;

const unsigned long ALERT_COOLDOWN_MS =
  30UL * 60UL * 1000UL;

const unsigned long SENSOR_INTERVAL_MS = 2000;
const unsigned long TG_POLL_MS = 5000;
const unsigned long HIST_INTERVAL_MS = 60000;

// Timezone
// UK example: "GMT0BST,M3.5.0/1,M10.5.0"
const char* TZ_INFO = "EST5EDT,M3.2.0/2,M11.1.0/2";
const char* NTP1 = "pool.ntp.org";
const char* NTP2 = "time.nist.gov";

// ==================================================

enum Mode {
  M_OFF = 0,
  M_ON = 1,
  M_AUTO = 2
};

const char* modeName(Mode m) {
  return (m == M_OFF) ? "off" : (m == M_ON ? "on" : "auto");
}

const int A_COLD_AMB  = 0;
const int A_COLD_FLR  = 1;
const int A_HOT_AMB   = 2;
const int A_HOT_FLR   = 3;
const int A_SENSOR_F  = 4;
const int A_SENSOR_A  = 5;
const int A_HEAT_LONG = 6;
const int A_NOHEAT    = 7;
const int A_HUM_HIGH  = 8;
const int A_HUM_LOW   = 9;
const int ALERT_COUNT = 10;

// ---------------- Cameras ----------------

struct CamCfg {
  const char* label;
  uint8_t pin;
  const char* kOn;
  const char* kEn;
  const char* kDay;
  const char* kNight;
  bool on;
  bool schedEn;
  int day;
  int night;
};

const int CAM_COUNT = 2;

CamCfg cams[CAM_COUNT] = {
  {
    "Play Cam",
    RELAY_CAM_PLAY,
    "cam",
    "cs_en",
    "cs_day",
    "cs_ngt",
    true,
    true,
    7,
    21
  },
  {
    "Bed Cam",
    RELAY_CAM_BED,
    "bcam",
    "bs_en",
    "bs_day",
    "bs_ngt",
    true,
    false,
    21,
    7
  }
};

OneWire oneWire(ONEWIRE_PIN);
DallasTemperature sensors(&oneWire);
WebServer server(80);
Preferences prefs;

Mode heatMode = M_AUTO;
bool heatOn = false;

float floorT = NAN;
float ambT = NAN;
float humT = NAN;
float shedT = NAN;

float onBelow = 18.0;
float offAbove = 21.0;

// Editable alert thresholds
float floorColdAlert = 16.0;
float floorHotAlert  = 35.0;
float ambColdAlert   = 15.0;
float ambHotAlert    = 27.0;
float humLowAlert    = 30.0;
float humHighAlert   = 75.0;

unsigned long lastSensor = 0;
unsigned long lastTg = 0;
unsigned long lastHist = 0;
unsigned long lastWiFiTry = 0;

long tgOffset = 0;

// Heat-run tracking
unsigned long heatOnSince = 0;
float floorAtHeatStart = NAN;

// Wi-Fi outage tracking
bool wifiWasDown = false;
unsigned long wifiDownSince = 0;
bool netServicesStarted = false;

unsigned long lastAlert[ALERT_COUNT] = {0};
bool alertSent[ALERT_COUNT] = {false};

// SHT40 state
bool shtRequested = false;
int shtFailCount = 0;

// Browser firmware upload state
bool uploadAuthorized = false;
bool uploadDone = false;
String uploadError = "";

const int HIST_SIZE = 1440;

float histFloor[HIST_SIZE];
float histAmb[HIST_SIZE];
float histShed[HIST_SIZE];
float histHum[HIST_SIZE];

uint8_t histHeat[HIST_SIZE];
uint8_t histCam[CAM_COUNT][HIST_SIZE];

uint32_t histTs[HIST_SIZE];
int histHead = 0;

// ---------------- Helpers ----------------

void setRelay(int pin, bool on) {
  digitalWrite(
    pin,
    (on ^ RELAY_ACTIVE_LOW) ? HIGH : LOW
  );
}

void applyOutputs() {
  setRelay(RELAY_HEAT, heatOn);

  for (int i = 0; i < CAM_COUNT; i++) {
    setRelay(cams[i].pin, cams[i].on);
  }
}

String fmtTemp(float t) {
  if (isnan(t)) return "ERR";
  return String(t, 1) + " C";
}

String fmtHum(float h) {
  if (isnan(h)) return "ERR";
  return String(h, 0) + " %";
}

String fmtDuration(unsigned long ms) {
  unsigned long m = ms / 60000UL;
  return String(m / 60) + "h " + String(m % 60) + "m";
}

bool ensureAuth() {
  if (!server.authenticate(DASH_USER, DASH_PASS)) {
    server.requestAuthentication();
    return false;
  }

  return true;
}

String resetReasonText() {
  switch (esp_reset_reason()) {
    case ESP_RST_POWERON:
      return "power on";

    case ESP_RST_SW:
      return "software reset";

    case ESP_RST_PANIC:
      return "crash (panic)";

    case ESP_RST_INT_WDT:
    case ESP_RST_TASK_WDT:
    case ESP_RST_WDT:
      return "watchdog";

    case ESP_RST_BROWNOUT:
      return "brownout";

    case ESP_RST_DEEPSLEEP:
      return "deep sleep wake";

    default:
      return "other/unknown";
  }
}

int camIdFromName(const String& s) {
  if (s == "play") return 0;
  if (s == "bed") return 1;
  return -1;
}

String tokenAt(const String& s, int n) {
  int idx = 0;
  int start = 0;
  int len = s.length();

  for (int i = 0; i <= len; i++) {
    if (i == len || s[i] == ' ') {
      if (i > start) {
        if (idx == n) return s.substring(start, i);
        idx++;
      }

      start = i + 1;
    }
  }

  return "";
}

// ---------------- Persistence ----------------

void saveSettings() {
  prefs.putUChar("mode", (uint8_t)heatMode);
  prefs.putFloat("on", onBelow);
  prefs.putFloat("off", offAbove);

  for (int i = 0; i < CAM_COUNT; i++) {
    prefs.putBool(cams[i].kOn, cams[i].on);
    prefs.putBool(cams[i].kEn, cams[i].schedEn);
    prefs.putInt(cams[i].kDay, cams[i].day);
    prefs.putInt(cams[i].kNight, cams[i].night);
  }

  prefs.putFloat("f_cold", floorColdAlert);
  prefs.putFloat("f_hot", floorHotAlert);
  prefs.putFloat("a_cold", ambColdAlert);
  prefs.putFloat("a_hot", ambHotAlert);
  prefs.putFloat("h_low", humLowAlert);
  prefs.putFloat("h_high", humHighAlert);
}

void loadSettings() {
  heatMode = (Mode)prefs.getUChar("mode", M_AUTO);

  if ((int)heatMode > M_AUTO) {
    heatMode = M_AUTO;
  }

  onBelow = prefs.getFloat("on", DEF_ON_BELOW);
  offAbove = prefs.getFloat("off", DEF_OFF_ABOVE);

  for (int i = 0; i < CAM_COUNT; i++) {
    CamCfg& c = cams[i];

    c.on = prefs.getBool(c.kOn, c.on);
    c.schedEn = prefs.getBool(c.kEn, c.schedEn);
    c.day = prefs.getInt(c.kDay, c.day);
    c.night = prefs.getInt(c.kNight, c.night);

    if (c.day < 0 || c.day > 23) {
      c.day = (i == 0) ? 7 : 21;
    }

    if (c.night < 0 || c.night > 23) {
      c.night = (i == 0) ? 21 : 7;
    }
  }

  floorColdAlert = prefs.getFloat("f_cold", DEF_FLOOR_COLD);
  floorHotAlert = prefs.getFloat("f_hot", DEF_FLOOR_HOT);
  ambColdAlert = prefs.getFloat("a_cold", DEF_AMB_COLD);
  ambHotAlert = prefs.getFloat("a_hot", DEF_AMB_HOT);
  humLowAlert = prefs.getFloat("h_low", DEF_HUM_LOW);
  humHighAlert = prefs.getFloat("h_high", DEF_HUM_HIGH);

  if (onBelow < 5 || onBelow > 32) {
    onBelow = DEF_ON_BELOW;
  }

  if (offAbove < 6 || offAbove > 36 || offAbove <= onBelow) {
    offAbove = DEF_OFF_ABOVE;
  }

  if (floorColdAlert < 0 || floorColdAlert > 35) {
    floorColdAlert = DEF_FLOOR_COLD;
  }

  if (
    floorHotAlert < 5 ||
    floorHotAlert > 45 ||
    floorHotAlert <= floorColdAlert
  ) {
    floorHotAlert = DEF_FLOOR_HOT;
  }

  if (ambColdAlert < -10 || ambColdAlert > 30) {
    ambColdAlert = DEF_AMB_COLD;
  }

  if (
    ambHotAlert < 10 ||
    ambHotAlert > 45 ||
    ambHotAlert <= ambColdAlert
  ) {
    ambHotAlert = DEF_AMB_HOT;
  }

  if (humLowAlert < 0 || humLowAlert > 100) {
    humLowAlert = DEF_HUM_LOW;
  }

  if (
    humHighAlert < 0 ||
    humHighAlert > 100 ||
    humHighAlert <= humLowAlert
  ) {
    humHighAlert = DEF_HUM_HIGH;
  }
}

// ---------------- Telegram ----------------

bool tgSend(const String& text) {
  if (WiFi.status() != WL_CONNECTED) return false;

  WiFiClientSecure client;
  client.setInsecure();

  HTTPClient http;

  String url =
    String("https://api.telegram.org/bot") +
    BOT_TOKEN +
    "/sendMessage";

  http.begin(client, url);
  http.addHeader("Content-Type", "application/json");

  JsonDocument d;
  d["chat_id"] = CHAT_ID;
  d["text"] = text;

  String body;
  serializeJson(d, body);

  int code = http.POST(body);
  http.end();

  return code == 200;
}

void raiseAlert(int id, const String& msg) {
  unsigned long now = millis();

  if (
    !alertSent[id] ||
    now - lastAlert[id] > ALERT_COOLDOWN_MS
  ) {
    if (tgSend(msg)) {
      alertSent[id] = true;
      lastAlert[id] = now;
    }
  }
}

void clearAlert(int id, const String& okMsg) {
  if (alertSent[id]) {
    alertSent[id] = false;
    tgSend(okMsg);
  }
}

String statusText() {
  String s = "Hutch status\n";

  s += "Floor: " + fmtTemp(floorT) + "\n";
  s += "Ambient: " + fmtTemp(ambT) + "\n";
  s += "Shed ambient: " + fmtTemp(shedT) + "\n";
  s += "Humidity: " + fmtHum(humT) + "\n";

  s += String("Heat: ") +
       (heatOn ? "ON" : "OFF") +
       " (" +
       modeName(heatMode) +
       ")";

  if (heatOn) {
    s += " for " + fmtDuration(millis() - heatOnSince);
  }

  s += "\n";

  for (int i = 0; i < CAM_COUNT; i++) {
    s += String(cams[i].label) +
         ": " +
         (cams[i].on ? "ON" : "OFF") +
         " (schedule " +
         (cams[i].schedEn ? "ON " : "OFF ") +
         String(cams[i].day) +
         ":00-" +
         String(cams[i].night) +
         ":00)\n";
  }

  s += "Auto range (floor): ON<" +
       String(onBelow, 1) +
       " OFF>" +
       String(offAbove, 1) +
       " C\n";

  s += "Floor alerts cold/hot: " +
       String(floorColdAlert, 1) +
       "/" +
       String(floorHotAlert, 1) +
       " C\n";

  s += "Ambient alerts cold/hot: " +
       String(ambColdAlert, 1) +
       "/" +
       String(ambHotAlert, 1) +
       " C\n";

  s += "Humidity alerts low/high: " +
       String(humLowAlert, 0) +
       "/" +
       String(humHighAlert, 0) +
       " %\n";

  s += "Uptime: " + fmtDuration(millis()) + "\n";
  s += "WiFi RSSI: " + String(WiFi.RSSI()) + " dBm";

  return s;
}

void handleCamCmd(const String& low) {
  int id = camIdFromName(tokenAt(low, 1));
  String arg = tokenAt(low, 2);

  if (id < 0 || (arg != "on" && arg != "off")) {
    tgSend("Use /cam play|bed on|off");
    return;
  }

  cams[id].on = (arg == "on");

  applyOutputs();
  saveSettings();

  tgSend(
    String(cams[id].label) +
    ": " +
    (cams[id].on ? "ON" : "OFF")
  );
}

void handleScheduleCmd(const String& low) {
  int id = camIdFromName(tokenAt(low, 1));
  String a = tokenAt(low, 2);
  String b = tokenAt(low, 3);

  if (id < 0 || a == "") {
    tgSend(
      "Use /schedule play|bed on|off or "
      "/schedule play|bed <dayHour> <nightHour>"
    );
    return;
  }

  CamCfg& c = cams[id];

  if (a == "on" || a == "off") {
    c.schedEn = (a == "on");
    saveSettings();

    tgSend(
      String(c.label) +
      " schedule " +
      (c.schedEn ? "enabled" : "disabled")
    );
    return;
  }

  if (b == "") {
    tgSend(
      "Use /schedule " +
      tokenAt(low, 1) +
      " <dayHour> <nightHour>, e.g. 7 21"
    );
    return;
  }

  int day = a.toInt();
  int night = b.toInt();

  if (day < 0 || day > 23 || night < 0 || night > 23) {
    tgSend("Hours must be 0..23");
    return;
  }

  c.day = day;
  c.night = night;
  c.schedEn = true;

  saveSettings();

  tgSend(
    String(c.label) +
    " schedule: ON " +
    String(day) +
    ":00, OFF " +
    String(night) +
    ":00"
  );
}

void pollTelegram() {
  if (WiFi.status() != WL_CONNECTED) return;

  WiFiClientSecure client;
  client.setInsecure();

  HTTPClient http;
  http.setTimeout(5000);

  String url =
    String("https://api.telegram.org/bot") +
    BOT_TOKEN +
    "/getUpdates?timeout=0&offset=" +
    String(tgOffset);

  http.begin(client, url);

  int code = http.GET();

  if (code == 200) {
    JsonDocument doc;

    if (!deserializeJson(doc, http.getString())) {
      for (JsonObject u : doc["result"].as<JsonArray>()) {
        tgOffset = u["update_id"].as<long>() + 1;

        char chatBuf[24] = "";

        if (!u["message"]["chat"]["id"].isNull()) {
          long long cid =
            u["message"]["chat"]["id"].as<long long>();

          snprintf(chatBuf, sizeof(chatBuf), "%lld", cid);
        }

        if (String(chatBuf) != String(CHAT_ID)) continue;

        String t = u["message"]["text"] | "";
        t.trim();

        String low = t;
        low.toLowerCase();

        String cmd = tokenAt(low, 0);

        if (cmd == "/status") {
          tgSend(statusText());

        } else if (cmd == "/heat") {
          String arg = tokenAt(low, 1);

          if (arg == "auto") {
            heatMode = M_AUTO;
          } else if (arg == "on") {
            heatMode = M_ON;
          } else if (arg == "off") {
            heatMode = M_OFF;
          } else {
            tgSend("Use /heat on|off|auto");
            continue;
          }

          saveSettings();
          controlHeat();

          tgSend(
            "Heat mode: " + String(modeName(heatMode))
          );

        } else if (cmd == "/cam") {
          handleCamCmd(low);

        } else if (cmd == "/schedule") {
          handleScheduleCmd(low);

        } else if (cmd == "/start" || cmd == "/help") {
          tgSend(
            "Commands:\n"
            "/status\n"
            "/heat on|off|auto\n"
            "/cam play|bed on|off\n"
            "/schedule play|bed on|off\n"
            "/schedule play|bed <dayHour> <nightHour>"
          );
        }
      }
    }
  }

  http.end();
}

// ---------------- Sensors / Control ----------------

uint8_t shtCrc(const uint8_t* d, int n) {
  uint8_t crc = 0xFF;

  for (int i = 0; i < n; i++) {
    crc ^= d[i];

    for (int b = 0; b < 8; b++) {
      crc = (crc & 0x80)
        ? (uint8_t)((crc << 1) ^ 0x31)
        : (uint8_t)(crc << 1);
    }
  }

  return crc;
}

// Start a high-precision SHT40 measurement (~9 ms).
void shtStart() {
  Wire.beginTransmission(SHT_ADDR);
  Wire.write(0xFD);
  shtRequested = (Wire.endTransmission() == 0);
}

bool shtRead(float& t, float& h) {
  if (!shtRequested) return false;

  if (Wire.requestFrom((uint16_t)SHT_ADDR, (size_t)6) != 6) {
    return false;
  }

  uint8_t b[6];

  for (int i = 0; i < 6; i++) {
    b[i] = Wire.read();
  }

  if (
    shtCrc(b, 2) != b[2] ||
    shtCrc(b + 3, 2) != b[5]
  ) {
    return false;
  }

  uint16_t rawT = ((uint16_t)b[0] << 8) | b[1];
  uint16_t rawH = ((uint16_t)b[3] << 8) | b[4];

  t = -45.0f + 175.0f * rawT / 65535.0f;
  h = -6.0f + 125.0f * rawH / 65535.0f;

  if (h < 0) h = 0;
  if (h > 100) h = 100;

  return true;
}

void readSensors() {
  // Read results from the previous conversion.
  float f = sensors.getTempC(ADDR_FLOOR);

  floorT =
    (f == DEVICE_DISCONNECTED_C || f < -50 || f > 100)
    ? NAN
    : f;

  float s = shedSensorFound
    ? sensors.getTempC(ADDR_SHED)
    : DEVICE_DISCONNECTED_C;

  shedT =
    (s == DEVICE_DISCONNECTED_C || s < -50 || s > 100)
    ? NAN
    : s;

  // Start the next conversion for both DS18B20s.
  sensors.requestTemperatures();

  float t;
  float h;

  if (shtRead(t, h)) {
    ambT = t;
    humT = h;
    shtFailCount = 0;

  } else {
    ambT = NAN;
    humT = NAN;

    if (++shtFailCount >= 5) {
      Wire.end();
      Wire.begin(I2C_SDA_PIN, I2C_SCL_PIN);
      Wire.setClock(I2C_HZ);
      shtFailCount = 0;
    }
  }

  shtStart();
}

bool sameDeviceAddress(const uint8_t* a, const uint8_t* b) {
  for (int i = 0; i < 8; i++) {
    if (a[i] != b[i]) return false;
  }

  return true;
}

bool findShedSensor() {
  DeviceAddress address;

  for (uint8_t i = 0; i < sensors.getDeviceCount(); i++) {
    if (!sensors.getAddress(address, i) || address[0] != 0x28) {
      continue;
    }

    if (sameDeviceAddress(address, ADDR_FLOOR)) {
      continue;
    }

    memcpy(ADDR_SHED, address, sizeof(DeviceAddress));
    return true;
  }

  return false;
}

void applyCameraSchedules() {
  struct tm ti;
  bool haveTime = false;
  bool changed = false;

  for (int i = 0; i < CAM_COUNT; i++) {
    CamCfg& c = cams[i];

    if (!c.schedEn) continue;

    if (!haveTime) {
      if (!getLocalTime(&ti, 50)) return;
      haveTime = true;
    }

    int h = ti.tm_hour;
    bool shouldOn;

    if (c.day < c.night) {
      shouldOn = (h >= c.day && h < c.night);
    } else if (c.day > c.night) {
      shouldOn = (h >= c.day || h < c.night);
    } else {
      shouldOn = false;
    }

    if (c.on != shouldOn) {
      c.on = shouldOn;
      changed = true;
    }
  }

  if (changed) applyOutputs();
}

void controlHeat() {
  bool want = heatOn;

  if (heatMode == M_OFF) {
    want = false;

  } else if (heatMode == M_ON) {
    want = true;

  } else {
    // AUTO uses the FLOOR sensor.
    if (isnan(floorT)) {
      want = false;
    } else if (floorT < onBelow) {
      want = true;
    } else if (floorT > offAbove) {
      want = false;
    }
  }

  // This cutoff requires a valid floor reading.
  if (!isnan(floorT) && floorT > floorHotAlert) {
    want = false;
  }

  if (want != heatOn) {
    heatOn = want;

    if (heatOn) {
      heatOnSince = millis();
      floorAtHeatStart = floorT;
    }

    applyOutputs();
  }
}

void processAlerts() {
  if (isnan(floorT)) {
    raiseAlert(
      A_SENSOR_F,
      "SENSOR FAILURE: floor sensor not responding. "
      "AUTO heating disabled."
    );
  } else {
    clearAlert(A_SENSOR_F, "Floor sensor back online");
  }

  if (isnan(ambT)) {
    raiseAlert(
      A_SENSOR_A,
      "SENSOR FAILURE: SHT40 (ambient/humidity) not responding."
    );
  } else {
    clearAlert(A_SENSOR_A, "SHT40 sensor back online");
  }

  if (!isnan(floorT)) {
    if (floorT < floorColdAlert) {
      raiseAlert(
        A_COLD_FLR,
        "Floor too cold: " +
        String(floorT, 1) +
        " C (threshold " +
        String(floorColdAlert, 1) +
        ")"
      );
    } else if (floorT > floorColdAlert + 1.0) {
      clearAlert(
        A_COLD_FLR,
        "Floor recovered: " + String(floorT, 1) + " C"
      );
    }

    if (floorT > floorHotAlert) {
      raiseAlert(
        A_HOT_FLR,
        "Floor too hot: " +
        String(floorT, 1) +
        " C. Heat forced OFF."
      );
    } else if (floorT < floorHotAlert - 2.0) {
      clearAlert(
        A_HOT_FLR,
        "Floor no longer hot: " + String(floorT, 1) + " C"
      );
    }
  }

  if (!isnan(ambT)) {
    if (ambT < ambColdAlert) {
      raiseAlert(
        A_COLD_AMB,
        "Ambient too cold: " +
        String(ambT, 1) +
        " C (threshold " +
        String(ambColdAlert, 1) +
        ")"
      );
    } else if (ambT > ambColdAlert + 1.0) {
      clearAlert(
        A_COLD_AMB,
        "Ambient recovered: " + String(ambT, 1) + " C"
      );
    }

    if (ambT > ambHotAlert) {
      raiseAlert(
        A_HOT_AMB,
        "Ambient too hot: " + String(ambT, 1) + " C"
      );
    } else if (ambT < ambHotAlert - 1.0) {
      clearAlert(
        A_HOT_AMB,
        "Ambient no longer hot: " + String(ambT, 1) + " C"
      );
    }
  }

  if (!isnan(humT)) {
    if (humT > humHighAlert) {
      raiseAlert(
        A_HUM_HIGH,
        "Humidity too high: " +
        String(humT, 0) +
        " % (threshold " +
        String(humHighAlert, 0) +
        ")"
      );
    } else if (humT < humHighAlert - 3.0) {
      clearAlert(
        A_HUM_HIGH,
        "Humidity back to normal: " +
        String(humT, 0) +
        " %"
      );
    }

    if (humT < humLowAlert) {
      raiseAlert(
        A_HUM_LOW,
        "Humidity too low: " +
        String(humT, 0) +
        " % (threshold " +
        String(humLowAlert, 0) +
        ")"
      );
    } else if (humT > humLowAlert + 3.0) {
      clearAlert(
        A_HUM_LOW,
        "Humidity back to normal: " +
        String(humT, 0) +
        " %"
      );
    }
  }

  if (heatOn && (millis() - heatOnSince > HEAT_MAX_RUN_MS)) {
    raiseAlert(
      A_HEAT_LONG,
      "Heat pad ON continuously for " +
      fmtDuration(millis() - heatOnSince) +
      "."
    );
  } else if (!heatOn) {
    clearAlert(
      A_HEAT_LONG,
      "Heat pad now OFF (continuous-run alert cleared)"
    );
  }

  if (
    heatOn &&
    !isnan(floorT) &&
    !isnan(floorAtHeatStart) &&
    (millis() - heatOnSince > NOHEAT_CHECK_MS) &&
    (floorT < floorAtHeatStart + NOHEAT_MIN_RISE_C) &&
    (floorT < onBelow)
  ) {
    raiseAlert(
      A_NOHEAT,
      "Heat ON " +
      fmtDuration(millis() - heatOnSince) +
      " but floor not warming (" +
      fmtTemp(floorAtHeatStart) +
      " -> " +
      fmtTemp(floorT) +
      "). Check pad/relay/power."
    );

  } else if (!heatOn || (!isnan(floorT) && floorT >= onBelow)) {
    clearAlert(A_NOHEAT, "Heating response normal again");
  }
}

void checkWiFiRecovery() {
  if (WiFi.status() != WL_CONNECTED) {
    if (!wifiWasDown) {
      wifiWasDown = true;
      wifiDownSince = millis();
    }

  } else if (wifiWasDown) {
    wifiWasDown = false;

    tgSend(
      "WiFi reconnected after " +
      fmtDuration(millis() - wifiDownSince)
    );
  }
}

// ---------------- History ----------------

void addHistoryPoint() {
  uint32_t nowEpoch = (uint32_t)time(nullptr);

  if (nowEpoch < 100000) return;

  histFloor[histHead] = floorT;
  histAmb[histHead] = ambT;
  histShed[histHead] = shedT;
  histHum[histHead] = humT;
  histHeat[histHead] = heatOn ? 1 : 0;

  for (int i = 0; i < CAM_COUNT; i++) {
    histCam[i][histHead] = cams[i].on ? 1 : 0;
  }

  histTs[histHead] = nowEpoch;
  histHead = (histHead + 1) % HIST_SIZE;
}

// ---------------- Firmware upload ----------------

void handleFirmwareUpload() {
  HTTPUpload& up = server.upload();

  if (up.status == UPLOAD_FILE_START) {
    uploadDone = false;
    uploadError = "";

    uploadAuthorized =
      server.authenticate(DASH_USER, DASH_PASS);

    if (!uploadAuthorized) return;

    if (!up.filename.endsWith(".bin")) {
      uploadError = "file must be a .bin";
      uploadAuthorized = false;
      return;
    }

    // The main loop is blocked during upload.
    heatOn = false;
    applyOutputs();

    if (!Update.begin(UPDATE_SIZE_UNKNOWN)) {
      uploadError = Update.errorString();
      uploadAuthorized = false;
    }

  } else if (up.status == UPLOAD_FILE_WRITE) {
    if (
      uploadAuthorized &&
      Update.write(up.buf, up.currentSize) != up.currentSize
    ) {
      uploadError = Update.errorString();
      uploadAuthorized = false;
      Update.abort();
    }

  } else if (up.status == UPLOAD_FILE_END) {
    if (uploadAuthorized) {
      if (Update.end(true)) {
        uploadDone = true;
      } else {
        uploadError = Update.errorString();
      }
    }

  } else if (up.status == UPLOAD_FILE_ABORTED) {
    if (uploadAuthorized) Update.abort();
    uploadAuthorized = false;
  }
}

// ---------------- Web ----------------

const char APP_MANIFEST[] PROGMEM = R"MANIFEST(
{"name":"Piggie REAL-SAFE Monitor","short_name":"Piggie RSM","start_url":"/","scope":"/","display":"standalone","background_color":"#141c26","theme_color":"#0f161e","icons":[{"src":"/icon.svg","sizes":"any","type":"image/svg+xml","purpose":"any maskable"}]}
)MANIFEST";

const char APP_ICON[] PROGMEM = R"SVG(
<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 512 512">
<rect width="512" height="512" rx="112" fill="#141c26"/>
<path d="M111 173 91 73l117 54M401 173l20-100-117 54" fill="#f3a7b6" stroke="#f3a7b6" stroke-linejoin="round" stroke-width="24"/>
<circle cx="256" cy="275" r="166" fill="#f3a7b6"/>
<ellipse cx="198" cy="250" rx="17" ry="24" fill="#263647"/>
<ellipse cx="314" cy="250" rx="17" ry="24" fill="#263647"/>
<ellipse cx="256" cy="315" rx="62" ry="43" fill="#e6849a"/>
<circle cx="235" cy="312" r="8" fill="#263647"/>
<circle cx="277" cy="312" r="8" fill="#263647"/>
</svg>
)SVG";

void setupWeb() {
  server.on("/manifest.webmanifest", []() {
    server.send_P(
      200,
      "application/manifest+json",
      APP_MANIFEST
    );
  });

  server.on("/icon.svg", []() {
    server.send_P(200, "image/svg+xml", APP_ICON);
  });

  server.on("/", []() {
    if (!ensureAuth()) return;
    server.send_P(200, "text/html", PAGE);
  });

  server.on("/api/status", []() {
    if (!ensureAuth()) return;

    JsonDocument d;

    if (isnan(floorT)) {
      d["floor"] = nullptr;
    } else {
      d["floor"] = floorT;
    }

    if (isnan(ambT)) {
      d["ambient"] = nullptr;
    } else {
      d["ambient"] = ambT;
    }

    if (isnan(shedT)) {
      d["shedAmbient"] = nullptr;
    } else {
      d["shedAmbient"] = shedT;
    }

    if (isnan(humT)) {
      d["hum"] = nullptr;
    } else {
      d["hum"] = humT;
    }

    d["heat"] = heatOn;
    d["mode"] = modeName(heatMode);
    d["onBelow"] = onBelow;
    d["offAbove"] = offAbove;

    d["floorCold"] = floorColdAlert;
    d["floorHot"] = floorHotAlert;
    d["ambCold"] = ambColdAlert;
    d["ambHot"] = ambHotAlert;
    d["humLow"] = humLowAlert;
    d["humHigh"] = humHighAlert;

    JsonArray ca = d["cams"].to<JsonArray>();

    for (int i = 0; i < CAM_COUNT; i++) {
      JsonObject o = ca.add<JsonObject>();

      o["name"] = cams[i].label;
      o["on"] = cams[i].on;
      o["schedEn"] = cams[i].schedEn;
      o["day"] = cams[i].day;
      o["night"] = cams[i].night;
    }

    String out;
    serializeJson(d, out);

    server.send(200, "application/json", out);
  });

  server.on("/api/mode", HTTP_POST, []() {
    if (!ensureAuth()) return;

    String v = server.arg("v");
    v.toLowerCase();

    if (v == "off") {
      heatMode = M_OFF;
    } else if (v == "on") {
      heatMode = M_ON;
    } else {
      heatMode = M_AUTO;
    }

    saveSettings();
    controlHeat();

    server.send(200, "text/plain", "ok");
  });

  server.on("/api/cam", HTTP_POST, []() {
    if (!ensureAuth()) return;

    int id = server.arg("id").toInt();

    if (id < 0 || id >= CAM_COUNT) {
      server.send(400, "text/plain", "bad camera");
      return;
    }

    cams[id].on = (server.arg("v") == "1");

    applyOutputs();
    saveSettings();

    server.send(200, "text/plain", "ok");
  });

  server.on("/api/range", HTTP_POST, []() {
    if (!ensureAuth()) return;

    float lo = server.arg("lo").toFloat();
    float hi = server.arg("hi").toFloat();

    if (!(lo > 5 && hi < 36 && hi > lo)) {
      server.send(400, "text/plain", "invalid range");
      return;
    }

    if (hi >= floorHotAlert) {
      server.send(
        400,
        "text/plain",
        "off temp must be below the floor hot cutoff (" +
        String(floorHotAlert, 1) +
        " C)"
      );
      return;
    }

    onBelow = lo;
    offAbove = hi;

    saveSettings();

    server.send(200, "text/plain", "ok");
  });

  server.on("/api/alerts", HTTP_POST, []() {
    if (!ensureAuth()) return;

    float fc = server.arg("fc").toFloat();
    float fh = server.arg("fh").toFloat();
    float ac = server.arg("ac").toFloat();
    float ah = server.arg("ah").toFloat();
    float hl = server.arg("hl").toFloat();
    float hh = server.arg("hh").toFloat();

    bool ok =
      (fc >= 0 && fc <= 35) &&
      (fh >= 5 && fh <= 45 && fh > fc) &&
      (ac >= -10 && ac <= 30) &&
      (ah >= 10 && ah <= 45 && ah > ac) &&
      (hl >= 0 && hl <= 100) &&
      (hh >= 0 && hh <= 100 && hh > hl);

    if (!ok) {
      server.send(400, "text/plain", "invalid values");
      return;
    }

    if (fh <= offAbove) {
      server.send(
        400,
        "text/plain",
        "floor hot cutoff must be above the heat off temp (" +
        String(offAbove, 1) +
        " C)"
      );
      return;
    }

    floorColdAlert = fc;
    floorHotAlert = fh;
    ambColdAlert = ac;
    ambHotAlert = ah;
    humLowAlert = hl;
    humHighAlert = hh;

    saveSettings();

    server.send(200, "text/plain", "ok");
  });

  server.on("/api/schedule", HTTP_POST, []() {
    if (!ensureAuth()) return;

    int id = server.arg("id").toInt();
    int day = server.arg("day").toInt();
    int night = server.arg("night").toInt();

    if (
      id < 0 ||
      id >= CAM_COUNT ||
      day < 0 ||
      day > 23 ||
      night < 0 ||
      night > 23
    ) {
      server.send(400, "text/plain", "bad values");
      return;
    }

    cams[id].schedEn = (server.arg("en") == "1");
    cams[id].day = day;
    cams[id].night = night;

    saveSettings();
    applyCameraSchedules();

    server.send(200, "text/plain", "ok");
  });

  server.on("/api/history", []() {
    if (!ensureAuth()) return;

    JsonDocument d;

    JsonArray ts = d["ts"].to<JsonArray>();
    JsonArray fl = d["floor"].to<JsonArray>();
    JsonArray am = d["amb"].to<JsonArray>();
    JsonArray sh = d["shed"].to<JsonArray>();
    JsonArray hu = d["hum"].to<JsonArray>();
    JsonArray ht = d["heat"].to<JsonArray>();
    JsonArray cp = d["play"].to<JsonArray>();
    JsonArray cb = d["bed"].to<JsonArray>();

    for (int i = 0; i < HIST_SIZE; i++) {
      int idx = (histHead + i) % HIST_SIZE;

      if (histTs[idx] == 0) continue;

      ts.add(histTs[idx]);

      if (isnan(histFloor[idx])) {
        fl.add(nullptr);
      } else {
        fl.add(histFloor[idx]);
      }

      if (isnan(histAmb[idx])) {
        am.add(nullptr);
      } else {
        am.add(histAmb[idx]);
      }

      if (isnan(histShed[idx])) {
        sh.add(nullptr);
      } else {
        sh.add(histShed[idx]);
      }

      if (isnan(histHum[idx])) {
        hu.add(nullptr);
      } else {
        hu.add(histHum[idx]);
      }

      ht.add(histHeat[idx]);
      cp.add(histCam[0][idx]);
      cb.add(histCam[1][idx]);
    }

    String out;
    serializeJson(d, out);

    server.send(200, "application/json", out);
  });

  server.on(
    "/api/update",
    HTTP_POST,
    []() {
      if (!ensureAuth()) return;

      if (!uploadDone) {
        server.send(
          500,
          "text/plain",
          "update failed: " +
          (
            uploadError.length()
            ? uploadError
            : String("no file received")
          )
        );
        return;
      }

      server.send(200, "text/plain", "ok, rebooting");
      delay(500);
      ESP.restart();
    },
    handleFirmwareUpload
  );

  server.begin();
}

// ---------------- OTA / Wi-Fi ----------------

void setupOTA() {
  ArduinoOTA.setHostname("guinea-hutch-esp32");
  ArduinoOTA.setPassword(OTA_PASS);

  ArduinoOTA.onStart([]() {
    tgSend("OTA update started");
  });

  ArduinoOTA.onEnd([]() {
    tgSend("OTA update complete, rebooting");
  });

  ArduinoOTA.onError([](ota_error_t error) {
    tgSend("OTA error code: " + String((int)error));
  });

  ArduinoOTA.begin();
}

void connectWiFi() {
  WiFi.mode(WIFI_STA);

  if (USE_STATIC_IP) {
    if (!WiFi.config(STATIC_IP, GATEWAY, SUBNET, DNS1, DNS2)) {
      Serial.println("Static IP config failed");
    }
  }

  WiFi.begin(WIFI_SSID, WIFI_PASS);

  unsigned long t0 = millis();

  while (
    WiFi.status() != WL_CONNECTED &&
    millis() - t0 < 20000
  ) {
    delay(250);
  }
}

void ensureWiFi() {
  if (WiFi.status() == WL_CONNECTED) return;

  if (millis() - lastWiFiTry > 15000) {
    lastWiFiTry = millis();
    WiFi.disconnect();
    WiFi.begin(WIFI_SSID, WIFI_PASS);
  }
}

void startNetServices() {
  if (netServicesStarted || WiFi.status() != WL_CONNECTED) {
    return;
  }

  netServicesStarted = true;

  Serial.print("WiFi IP: ");
  Serial.println(WiFi.localIP());

  configTzTime(TZ_INFO, NTP1, NTP2);
  setupOTA();
}

// ---------------- Setup / Loop ----------------

void setup() {
  Serial.begin(115200);
  delay(200);

  // Set levels before enabling outputs.
  setRelay(RELAY_HEAT, false);

  for (int i = 0; i < CAM_COUNT; i++) {
    setRelay(cams[i].pin, cams[i].on);
  }

  pinMode(RELAY_HEAT, OUTPUT);

  for (int i = 0; i < CAM_COUNT; i++) {
    pinMode(cams[i].pin, OUTPUT);
  }

  for (int i = 0; i < HIST_SIZE; i++) {
    histFloor[i] = NAN;
    histAmb[i] = NAN;
    histShed[i] = NAN;
    histHum[i] = NAN;

    histHeat[i] = 0;
    histTs[i] = 0;

    for (int c = 0; c < CAM_COUNT; c++) {
      histCam[c][i] = 0;
    }
  }

  prefs.begin("hutch", false);
  loadSettings();

  heatOn = false;
  applyOutputs();

  sensors.begin();
  sensors.setResolution(SENSOR_RESOLUTION_BITS);
  sensors.setWaitForConversion(false);

  shedSensorFound = findShedSensor();

  Wire.begin(I2C_SDA_PIN, I2C_SCL_PIN);
  Wire.setClock(I2C_HZ);

  Serial.printf(
    "Floor sensor (DS18B20): %s\n",
    sensors.isConnected(ADDR_FLOOR) ? "found" : "MISSING"
  );

  Serial.printf(
    "Shed ambient DS18B20: %s\n",
    shedSensorFound ? "found" : "MISSING"
  );

  Wire.beginTransmission(SHT_ADDR);

  Serial.printf(
    "SHT40 sensor: %s\n",
    Wire.endTransmission() == 0 ? "found" : "MISSING"
  );

  // Prime the first conversions.
  sensors.requestTemperatures();
  shtStart();
  delay(300);

  connectWiFi();
  startNetServices();

  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("WiFi not connected");
  }

  setupWeb();

  readSensors();
  applyCameraSchedules();
  controlHeat();
  applyOutputs();

  if (WiFi.status() == WL_CONNECTED) {
    tgSend(
      "Controller rebooted (reason: " +
      resetReasonText() +
      ")\nDashboard: http://" +
      WiFi.localIP().toString() +
      "\n\n" +
      statusText()
    );
  }
}

void loop() {
  server.handleClient();

  if (netServicesStarted) {
    ArduinoOTA.handle();
  }

  ensureWiFi();
  startNetServices();
  checkWiFiRecovery();

  unsigned long now = millis();

  if (now - lastSensor >= SENSOR_INTERVAL_MS) {
    lastSensor = now;

    readSensors();
    applyCameraSchedules();
    controlHeat();
    processAlerts();
  }

  if (now - lastHist >= HIST_INTERVAL_MS) {
    lastHist = now;
    addHistoryPoint();
  }

  if (now - lastTg >= TG_POLL_MS) {
    lastTg = now;
    pollTelegram();
  }
}