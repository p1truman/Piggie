/*
  Guinea Pig Hutch Controller - ESP32-S3
  Relay logic: ACTIVE HIGH (HIGH = ON)
  Relay 1 = heat pad (AUTO uses FLOOR sensor)
  Relay 2 = camera
  Web page is in index_html.h
*/

#include <WiFi.h>
#include <WebServer.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <OneWire.h>
#include <DallasTemperature.h>
#include <Preferences.h>
#include <ArduinoJson.h>
#include <ArduinoOTA.h>
#include <esp_system.h>
#include <time.h>
#include <math.h>
#include "index_html.h"

// ================== USER CONFIG ==================
const char* WIFI_SSID = "The Truman Network";
const char* WIFI_PASS = "Anth0ny1";

const char* BOT_TOKEN = "8840600263:AAE-Iynnb7zwCA8VUNDV5mnSCclFQCX7gq0";
const char* CHAT_ID   = "8610031496";

const char* DASH_USER = "admin";
const char* DASH_PASS = "Babie";

const char* OTA_PASS  = "Twinkie123";

// ---- Static IP (edit for your network) ----
const bool USE_STATIC_IP = true;
IPAddress STATIC_IP(192, 168, 1, 200);
IPAddress GATEWAY(192, 168, 1, 1);
IPAddress SUBNET(255, 255, 255, 0);
IPAddress DNS1(8, 8, 8, 8);
IPAddress DNS2(1, 1, 1, 1);

// Pins
#define ONEWIRE_PIN 4
#define RELAY_HEAT  5
#define RELAY_CAM   6
const bool RELAY_ACTIVE_LOW = false;

// DS18B20 hard-coded addresses
DeviceAddress ADDR_FLOOR   = {0x28, 0x24, 0xBE, 0xD3, 0x00, 0x00, 0x00, 0x10};
DeviceAddress ADDR_AMBIENT = {0x28, 0x9B, 0xEC, 0xD1, 0x00, 0x00, 0x00, 0x22};

// DS18B20 speed/precision tradeoff
// 9-bit = fastest (~94ms), 10-bit (~188ms), 11-bit (~375ms), 12-bit (~750ms)
const uint8_t SENSOR_RESOLUTION_BITS = 10;

// Auto heating thresholds (FLOOR based)
float DEF_ON_BELOW  = 18.0;
float DEF_OFF_ABOVE = 21.0;

// Alert defaults (dashboard editable)
float DEF_FLOOR_COLD = 16.0;
float DEF_FLOOR_HOT  = 35.0;
float DEF_AMB_COLD   = 15.0;
float DEF_AMB_HOT    = 27.0;

// 2h continuous heat runtime alert
const unsigned long HEAT_MAX_RUN_MS   = 2UL * 60UL * 60UL * 1000UL;

// Additional health alert
const unsigned long NOHEAT_CHECK_MS   = 45UL * 60UL * 1000UL;
const float         NOHEAT_MIN_RISE_C = 1.0;

const unsigned long ALERT_COOLDOWN_MS  = 30UL * 60UL * 1000UL;
const unsigned long SENSOR_INTERVAL_MS = 2000;   // was 5000, now 2s for snappier updates
const unsigned long TG_POLL_MS         = 5000;
const unsigned long HIST_INTERVAL_MS   = 60000;

// Timezone
// UK example: "GMT0BST,M3.5.0/1,M10.5.0"
const char* TZ_INFO = "EST5EDT,M3.2.0/2,M11.1.0/2";
const char* NTP1 = "pool.ntp.org";
const char* NTP2 = "time.nist.gov";

// Camera schedule defaults
bool DEF_CAM_SCHED_EN = true;
int DEF_DAY_START_H   = 7;
int DEF_NIGHT_START_H = 21;
// ==================================================

enum Mode { M_OFF = 0, M_ON = 1, M_AUTO = 2 };
const char* modeName(Mode m) { return (m == M_OFF) ? "off" : (m == M_ON ? "on" : "auto"); }

const int A_COLD_AMB  = 0;
const int A_COLD_FLR  = 1;
const int A_HOT_AMB   = 2;
const int A_HOT_FLR   = 3;
const int A_SENSOR_F  = 4;
const int A_SENSOR_A  = 5;
const int A_HEAT_LONG = 6;
const int A_NOHEAT    = 7;
const int ALERT_COUNT = 8;

OneWire oneWire(ONEWIRE_PIN);
DallasTemperature sensors(&oneWire);
WebServer server(80);
Preferences prefs;

Mode heatMode = M_AUTO;
bool camOn = true;
bool heatOn = false;

float floorT = NAN;
float ambT = NAN;
float onBelow = 18.0;
float offAbove = 21.0;

// Editable alert thresholds
float floorColdAlert = 16.0;
float floorHotAlert  = 35.0;
float ambColdAlert   = 15.0;
float ambHotAlert    = 27.0;

bool camScheduleEnabled = true;
int camDayStartHour = 7;
int camNightStartHour = 21;

unsigned long lastSensor = 0, lastTg = 0, lastHist = 0, lastWiFiTry = 0;
long tgOffset = 0;

// Heat-run tracking
unsigned long heatOnSince = 0;
float floorAtHeatStart = NAN;

// Wi-Fi outage tracking
bool wifiWasDown = false;
unsigned long wifiDownSince = 0;

unsigned long lastAlert[ALERT_COUNT] = {0};
bool alertSent[ALERT_COUNT] = {false};

const int HIST_SIZE = 1440;
float histFloor[HIST_SIZE];
float histAmb[HIST_SIZE];
uint8_t histHeat[HIST_SIZE];
uint8_t histCam[HIST_SIZE];
uint32_t histTs[HIST_SIZE];
int histHead = 0;

// ---------------- Helpers ----------------
void setRelay(int pin, bool on) {
  digitalWrite(pin, (on ^ RELAY_ACTIVE_LOW) ? HIGH : LOW);
}
void applyOutputs() {
  setRelay(RELAY_HEAT, heatOn);
  setRelay(RELAY_CAM, camOn);
}
String fmtTemp(float t) {
  if (isnan(t)) return "ERR";
  return String(t, 1) + " C";
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
    case ESP_RST_POWERON:  return "power on";
    case ESP_RST_SW:       return "software reset";
    case ESP_RST_PANIC:    return "crash (panic)";
    case ESP_RST_INT_WDT:
    case ESP_RST_TASK_WDT:
    case ESP_RST_WDT:      return "watchdog";
    case ESP_RST_BROWNOUT: return "brownout";
    case ESP_RST_DEEPSLEEP:return "deep sleep wake";
    default:               return "other/unknown";
  }
}

// ---------------- Persistence ----------------
void saveSettings() {
  prefs.putUChar("mode", (uint8_t)heatMode);
  prefs.putBool("cam", camOn);
  prefs.putFloat("on", onBelow);
  prefs.putFloat("off", offAbove);
  prefs.putBool("cs_en", camScheduleEnabled);
  prefs.putInt("cs_day", camDayStartHour);
  prefs.putInt("cs_ngt", camNightStartHour);

  prefs.putFloat("f_cold", floorColdAlert);
  prefs.putFloat("f_hot",  floorHotAlert);
  prefs.putFloat("a_cold", ambColdAlert);
  prefs.putFloat("a_hot",  ambHotAlert);
}
void loadSettings() {
  heatMode = (Mode)prefs.getUChar("mode", M_AUTO);
  camOn = prefs.getBool("cam", true);

  onBelow = prefs.getFloat("on", DEF_ON_BELOW);
  offAbove = prefs.getFloat("off", DEF_OFF_ABOVE);

  camScheduleEnabled = prefs.getBool("cs_en", DEF_CAM_SCHED_EN);
  camDayStartHour = prefs.getInt("cs_day", DEF_DAY_START_H);
  camNightStartHour = prefs.getInt("cs_ngt", DEF_NIGHT_START_H);

  floorColdAlert = prefs.getFloat("f_cold", DEF_FLOOR_COLD);
  floorHotAlert  = prefs.getFloat("f_hot",  DEF_FLOOR_HOT);
  ambColdAlert   = prefs.getFloat("a_cold", DEF_AMB_COLD);
  ambHotAlert    = prefs.getFloat("a_hot",  DEF_AMB_HOT);

  if (onBelow < 5 || onBelow > 32) onBelow = DEF_ON_BELOW;
  if (offAbove < 6 || offAbove > 36 || offAbove <= onBelow) offAbove = DEF_OFF_ABOVE;

  if (camDayStartHour < 0 || camDayStartHour > 23) camDayStartHour = DEF_DAY_START_H;
  if (camNightStartHour < 0 || camNightStartHour > 23) camNightStartHour = DEF_NIGHT_START_H;

  if (floorColdAlert < 0 || floorColdAlert > 35) floorColdAlert = DEF_FLOOR_COLD;
  if (floorHotAlert < 5 || floorHotAlert > 45 || floorHotAlert <= floorColdAlert) floorHotAlert = DEF_FLOOR_HOT;
  if (ambColdAlert < -10 || ambColdAlert > 30) ambColdAlert = DEF_AMB_COLD;
  if (ambHotAlert < 10 || ambHotAlert > 45 || ambHotAlert <= ambColdAlert) ambHotAlert = DEF_AMB_HOT;
}

// ---------------- Telegram ----------------
bool tgSend(const String& text) {
  if (WiFi.status() != WL_CONNECTED) return false;
  WiFiClientSecure client;
  client.setInsecure();
  HTTPClient http;
  String url = String("https://api.telegram.org/bot") + BOT_TOKEN + "/sendMessage";
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
  if (!alertSent[id] || now - lastAlert[id] > ALERT_COOLDOWN_MS) {
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
  s += String("Heat: ") + (heatOn ? "ON" : "OFF") + " (" + modeName(heatMode) + ")";
  if (heatOn) s += " for " + fmtDuration(millis() - heatOnSince);
  s += "\n";
  s += String("Camera: ") + (camOn ? "ON" : "OFF") + "\n";
  s += "AUTO source: FLOOR\n";
  s += "Auto range: ON<" + String(onBelow,1) + " OFF>" + String(offAbove,1) + " C\n";
  s += "Alerts floor(cold/hot): " + String(floorColdAlert,1) + "/" + String(floorHotAlert,1) + " C\n";
  s += "Alerts amb(cold/hot): " + String(ambColdAlert,1) + "/" + String(ambHotAlert,1) + " C\n";
  s += "Schedule: " + String(camScheduleEnabled ? "ON " : "OFF ")
     + String(camDayStartHour) + ":00-" + String(camNightStartHour) + ":00\n";
  s += "Uptime: " + fmtDuration(millis()) + "\n";
  s += "WiFi RSSI: " + String(WiFi.RSSI()) + " dBm";
  return s;
}

void parseScheduleCmd(const String& low) {
  if (low == "/schedule on") { camScheduleEnabled = true; saveSettings(); tgSend("Camera schedule enabled"); return; }
  if (low == "/schedule off") { camScheduleEnabled = false; saveSettings(); tgSend("Camera schedule disabled"); return; }

  int p1 = low.indexOf(' ');
  if (p1 < 0) { tgSend("Use /schedule on|off or /schedule <dayHour> <nightHour>"); return; }
  String rest = low.substring(p1 + 1); rest.trim();
  int p2 = rest.indexOf(' ');
  if (p2 < 0) { tgSend("Use /schedule <dayHour> <nightHour>, e.g. /schedule 7 21"); return; }

  int day = rest.substring(0, p2).toInt();
  int night = rest.substring(p2 + 1).toInt();
  if (day < 0 || day > 23 || night < 0 || night > 23) { tgSend("Hours must be 0..23"); return; }

  camDayStartHour = day;
  camNightStartHour = night;
  camScheduleEnabled = true;
  saveSettings();
  tgSend("Schedule set: ON " + String(day) + ":00, OFF " + String(night) + ":00");
}

void pollTelegram() {
  if (WiFi.status() != WL_CONNECTED) return;

  WiFiClientSecure client;
  client.setInsecure();
  HTTPClient http;
  http.setTimeout(5000);

  String url = String("https://api.telegram.org/bot") + BOT_TOKEN + "/getUpdates?timeout=0&offset=" + String(tgOffset);
  http.begin(client, url);
  int code = http.GET();

  if (code == 200) {
    JsonDocument doc;
    if (!deserializeJson(doc, http.getString())) {
      for (JsonObject u : doc["result"].as<JsonArray>()) {
        tgOffset = u["update_id"].as<long>() + 1;

        char chatBuf[24] = "";
        if (!u["message"]["chat"]["id"].isNull()) {
          long long cid = u["message"]["chat"]["id"].as<long long>();
          snprintf(chatBuf, sizeof(chatBuf), "%lld", cid);
        }
        Serial.printf("Telegram msg from chat %s\n", chatBuf);
        if (String(chatBuf) != String(CHAT_ID)) continue;

        String t = u["message"]["text"] | "";
        t.trim();
        String low = t; low.toLowerCase();

        if (low.startsWith("/status")) {
          tgSend(statusText());
        } else if (low.startsWith("/heat")) {
          if (low.indexOf("auto") > 0) heatMode = M_AUTO;
          else if (low.indexOf(" on") > 0) heatMode = M_ON;
          else if (low.indexOf("off") > 0) heatMode = M_OFF;
          else { tgSend("Use /heat on|off|auto"); continue; }
          saveSettings();
          tgSend("Heat mode: " + String(modeName(heatMode)));
        } else if (low.startsWith("/cam")) {
          if (low.indexOf(" on") > 0) camOn = true;
          else if (low.indexOf("off") > 0) camOn = false;
          else { tgSend("Use /cam on|off"); continue; }
          applyOutputs();
          saveSettings();
          tgSend(String("Camera: ") + (camOn ? "ON" : "OFF"));
        } else if (low.startsWith("/schedule")) {
          parseScheduleCmd(low);
        } else if (low.startsWith("/start") || low.startsWith("/help")) {
          tgSend("Commands:\n/status\n/heat on|off|auto\n/cam on|off\n/schedule on|off\n/schedule <dayHour> <nightHour>");
        }
      }
    }
  }
  http.end();
}

// ---------------- Sensors / Control ----------------
// Non-blocking DS18B20 read:
// - getTempC() returns result of previous conversion
// - requestTemperatures() starts next conversion and returns immediately
void readSensors() {
  float f = sensors.getTempC(ADDR_FLOOR);
  float a = sensors.getTempC(ADDR_AMBIENT);

  floorT = (f == DEVICE_DISCONNECTED_C || f < -50 || f > 100) ? NAN : f;
  ambT   = (a == DEVICE_DISCONNECTED_C || a < -50 || a > 100) ? NAN : a;

  sensors.requestTemperatures(); // kick off next measurement
}

void applyCameraSchedule() {
  if (!camScheduleEnabled) return;
  struct tm ti;
  if (!getLocalTime(&ti, 50)) return;

  int h = ti.tm_hour;
  bool shouldOn;
  if (camDayStartHour < camNightStartHour) shouldOn = (h >= camDayStartHour && h < camNightStartHour);
  else if (camDayStartHour > camNightStartHour) shouldOn = (h >= camDayStartHour || h < camNightStartHour);
  else shouldOn = false;

  if (camOn != shouldOn) {
    camOn = shouldOn;
    applyOutputs();
  }
}

void controlHeat() {
  bool want = heatOn;

  if (heatMode == M_OFF) want = false;
  else if (heatMode == M_ON) want = true;
  else {
    // AUTO uses FLOOR sensor
    if (isnan(floorT)) want = false;
    else if (floorT < onBelow) want = true;
    else if (floorT > offAbove) want = false;
  }

  if (!isnan(floorT) && floorT > floorHotAlert) want = false;

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
  if (isnan(floorT)) raiseAlert(A_SENSOR_F, "SENSOR FAILURE: floor sensor not responding. AUTO disabled.");
  else clearAlert(A_SENSOR_F, "Floor sensor back online");

  if (isnan(ambT)) raiseAlert(A_SENSOR_A, "SENSOR FAILURE: ambient sensor not responding.");
  else clearAlert(A_SENSOR_A, "Ambient sensor back online");

  if (!isnan(floorT)) {
    if (floorT < floorColdAlert)
      raiseAlert(A_COLD_FLR, "Floor too cold: " + String(floorT,1) + " C (threshold " + String(floorColdAlert,1) + ")");
    else if (floorT > floorColdAlert + 1.0)
      clearAlert(A_COLD_FLR, "Floor recovered: " + String(floorT,1) + " C");

    if (floorT > floorHotAlert)
      raiseAlert(A_HOT_FLR, "Floor too hot: " + String(floorT,1) + " C. Heat forced OFF.");
    else if (floorT < floorHotAlert - 2.0)
      clearAlert(A_HOT_FLR, "Floor no longer hot: " + String(floorT,1) + " C");
  }

  if (!isnan(ambT)) {
    if (ambT < ambColdAlert)
      raiseAlert(A_COLD_AMB, "Ambient too cold: " + String(ambT,1) + " C (threshold " + String(ambColdAlert,1) + ")");
    else if (ambT > ambColdAlert + 1.0)
      clearAlert(A_COLD_AMB, "Ambient recovered: " + String(ambT,1) + " C");

    if (ambT > ambHotAlert)
      raiseAlert(A_HOT_AMB, "Ambient too hot: " + String(ambT,1) + " C");
    else if (ambT < ambHotAlert - 1.0)
      clearAlert(A_HOT_AMB, "Ambient no longer hot: " + String(ambT,1) + " C");
  }

  if (heatOn && (millis() - heatOnSince > HEAT_MAX_RUN_MS)) {
    raiseAlert(A_HEAT_LONG, "Heat pad ON continuously for " + fmtDuration(millis() - heatOnSince) + ".");
  } else if (!heatOn) {
    clearAlert(A_HEAT_LONG, "Heat pad now OFF (continuous-run alert cleared)");
  }

  if (heatOn && !isnan(floorT) && !isnan(floorAtHeatStart) &&
      (millis() - heatOnSince > NOHEAT_CHECK_MS) &&
      (floorT < floorAtHeatStart + NOHEAT_MIN_RISE_C) &&
      (floorT < onBelow)) {
    raiseAlert(A_NOHEAT, "Heat ON " + fmtDuration(millis() - heatOnSince) + " but floor not warming (" +
              fmtTemp(floorAtHeatStart) + " -> " + fmtTemp(floorT) + "). Check pad/relay/power.");
  } else if (!heatOn || (!isnan(floorT) && floorT >= onBelow)) {
    clearAlert(A_NOHEAT, "Heating response normal again");
  }
}

void checkWiFiRecovery() {
  if (WiFi.status() != WL_CONNECTED) {
    if (!wifiWasDown) { wifiWasDown = true; wifiDownSince = millis(); }
  } else if (wifiWasDown) {
    wifiWasDown = false;
    tgSend("WiFi reconnected after " + fmtDuration(millis() - wifiDownSince));
  }
}

// ---------------- History ----------------
void addHistoryPoint() {
  uint32_t nowEpoch = (uint32_t)time(nullptr);
  if (nowEpoch < 100000) return;
  histFloor[histHead] = floorT;
  histAmb[histHead] = ambT;
  histHeat[histHead] = heatOn ? 1 : 0;
  histCam[histHead] = camOn ? 1 : 0;
  histTs[histHead] = nowEpoch;
  histHead = (histHead + 1) % HIST_SIZE;
}

// ---------------- Web ----------------
void setupWeb() {
  server.on("/", []() {
    if (!ensureAuth()) return;
    server.send_P(200, "text/html", PAGE);
  });

  server.on("/api/status", []() {
    if (!ensureAuth()) return;
    JsonDocument d;
    if (isnan(floorT)) d["floor"] = nullptr; else d["floor"] = floorT;
    if (isnan(ambT)) d["ambient"] = nullptr; else d["ambient"] = ambT;
    d["heat"] = heatOn;
    d["mode"] = modeName(heatMode);
    d["cam"] = camOn;
    d["onBelow"] = onBelow;
    d["offAbove"] = offAbove;
    d["camSchedEn"] = camScheduleEnabled;
    d["camDay"] = camDayStartHour;
    d["camNight"] = camNightStartHour;
    d["autoSource"] = "floor";
    d["floorCold"] = floorColdAlert;
    d["floorHot"]  = floorHotAlert;
    d["ambCold"]   = ambColdAlert;
    d["ambHot"]    = ambHotAlert;
    String out;
    serializeJson(d, out);
    server.send(200, "application/json", out);
  });

  server.on("/api/mode", HTTP_POST, []() {
    if (!ensureAuth()) return;
    String v = server.arg("v"); v.toLowerCase();
    if (v == "off") heatMode = M_OFF;
    else if (v == "on") heatMode = M_ON;
    else heatMode = M_AUTO;
    saveSettings();
    controlHeat();
    server.send(200, "text/plain", "ok");
  });

  server.on("/api/cam", HTTP_POST, []() {
    if (!ensureAuth()) return;
    camOn = (server.arg("v") == "1");
    applyOutputs();
    saveSettings();
    server.send(200, "text/plain", "ok");
  });

  server.on("/api/range", HTTP_POST, []() {
    if (!ensureAuth()) return;
    float lo = server.arg("lo").toFloat();
    float hi = server.arg("hi").toFloat();
    if (lo > 5 && hi < 36 && hi > lo) {
      onBelow = lo; offAbove = hi;
      saveSettings();
      server.send(200, "text/plain", "ok");
    } else {
      server.send(400, "text/plain", "invalid range");
    }
  });

  server.on("/api/alerts", HTTP_POST, []() {
    if (!ensureAuth()) return;
    float fc = server.arg("fc").toFloat();
    float fh = server.arg("fh").toFloat();
    float ac = server.arg("ac").toFloat();
    float ah = server.arg("ah").toFloat();

    bool ok =
      (fc >= 0 && fc <= 35) &&
      (fh >= 5 && fh <= 45 && fh > fc) &&
      (ac >= -10 && ac <= 30) &&
      (ah >= 10 && ah <= 45 && ah > ac);

    if (!ok) {
      server.send(400, "text/plain", "invalid values");
      return;
    }

    floorColdAlert = fc;
    floorHotAlert  = fh;
    ambColdAlert   = ac;
    ambHotAlert    = ah;
    saveSettings();
    server.send(200, "text/plain", "ok");
  });

  server.on("/api/schedule", HTTP_POST, []() {
    if (!ensureAuth()) return;
    bool en = (server.arg("en") == "1");
    int day = server.arg("day").toInt();
    int night = server.arg("night").toInt();
    if (day < 0 || day > 23 || night < 0 || night > 23) {
      server.send(400, "text/plain", "bad values");
      return;
    }
    camScheduleEnabled = en;
    camDayStartHour = day;
    camNightStartHour = night;
    saveSettings();
    applyCameraSchedule();
    server.send(200, "text/plain", "ok");
  });

  server.on("/api/history", []() {
    if (!ensureAuth()) return;
    JsonDocument d;
    JsonArray ts = d["ts"].to<JsonArray>();
    JsonArray fl = d["floor"].to<JsonArray>();
    JsonArray am = d["amb"].to<JsonArray>();
    JsonArray ht = d["heat"].to<JsonArray>();
    JsonArray cm = d["cam"].to<JsonArray>();

    for (int i = 0; i < HIST_SIZE; i++) {
      int idx = (histHead + i) % HIST_SIZE;
      if (histTs[idx] == 0) continue;
      ts.add(histTs[idx]);
      if (isnan(histFloor[idx])) fl.add(nullptr); else fl.add(histFloor[idx]);
      if (isnan(histAmb[idx])) am.add(nullptr); else am.add(histAmb[idx]);
      ht.add(histHeat[idx]);
      cm.add(histCam[idx]);
    }

    String out;
    serializeJson(d, out);
    server.send(200, "application/json", out);
  });

  server.begin();
}

// ---------------- OTA / Wi-Fi ----------------
void setupOTA() {
  ArduinoOTA.setHostname("guinea-hutch-esp32");
  ArduinoOTA.setPassword(OTA_PASS);
  ArduinoOTA.onStart([]() { tgSend("OTA update started"); });
  ArduinoOTA.onEnd([]() { tgSend("OTA update complete, rebooting"); });
  ArduinoOTA.onError([](ota_error_t error) { tgSend("OTA error code: " + String((int)error)); });
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
  while (WiFi.status() != WL_CONNECTED && millis() - t0 < 20000) delay(250);
}

void ensureWiFi() {
  if (WiFi.status() == WL_CONNECTED) return;
  if (millis() - lastWiFiTry > 15000) {
    lastWiFiTry = millis();
    WiFi.disconnect();
    WiFi.begin(WIFI_SSID, WIFI_PASS);
  }
}

// ---------------- Setup / Loop ----------------
void setup() {
  Serial.begin(115200);
  delay(200);

  pinMode(RELAY_HEAT, OUTPUT);
  pinMode(RELAY_CAM, OUTPUT);

  heatOn = false;
  camOn = true;
  applyOutputs();

  for (int i = 0; i < HIST_SIZE; i++) {
    histFloor[i] = NAN; histAmb[i] = NAN;
    histHeat[i] = 0; histCam[i] = 0; histTs[i] = 0;
  }

  prefs.begin("hutch", false);
  loadSettings();

  sensors.begin();
  sensors.setResolution(SENSOR_RESOLUTION_BITS);
  sensors.setWaitForConversion(false);  // non-blocking mode

  Serial.printf("Floor sensor:   %s\n", sensors.isConnected(ADDR_FLOOR) ? "found" : "MISSING");
  Serial.printf("Ambient sensor: %s\n", sensors.isConnected(ADDR_AMBIENT) ? "found" : "MISSING");
  Serial.printf("DS18B20 resolution: %u-bit\n", SENSOR_RESOLUTION_BITS);

  // Prime first conversion so first read isn't stale/invalid
  sensors.requestTemperatures();
  delay(300);

  connectWiFi();
  if (WiFi.status() == WL_CONNECTED) {
    Serial.print("WiFi IP: ");
    Serial.println(WiFi.localIP());
    configTzTime(TZ_INFO, NTP1, NTP2);
    setupOTA();
  } else {
    Serial.println("WiFi not connected");
  }

  setupWeb();

  readSensors();           // gets first completed reading + starts next
  applyCameraSchedule();
  controlHeat();
  applyOutputs();

  if (WiFi.status() == WL_CONNECTED) {
    tgSend("Controller rebooted (reason: " + resetReasonText() + ")\nDashboard: http://" + WiFi.localIP().toString() + "\n\n" + statusText());
  }
}

void loop() {
  server.handleClient();
  ArduinoOTA.handle();
  ensureWiFi();
  checkWiFiRecovery();

  unsigned long now = millis();

  if (now - lastSensor >= SENSOR_INTERVAL_MS) {
    lastSensor = now;
    readSensors();
    applyCameraSchedule();
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