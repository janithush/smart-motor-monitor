/*
  =====================================================================
  ESP32 SMART INDUCTION MOTOR MONITORING & PREDICTIVE FAULT ALERT SYSTEM
  =====================================================================
  WEB-DASHBOARD REVISION
  Same sensing/filtering/trip core as the bench-validated sketch, now
  extended with:
    - Local HTTP dashboard on port 80 (stock WebServer, PROGMEM UI).
    - REST API: /api/status, /api/reset, /api/motor, /api/log (+fmt=html).
    - In-DRAM event ring buffer (event_log.h) powering the dashboard
      event table, CSV export and printable report.
    - Shared safety-interlocked actuation helpers: WEB commands and the
      physical buttons call ONE code path, preserving the 5 s soft-start
      and vibration startup grace windows.
    - Cooperative loop: vibration sampling tolerates Telegram/HTTP
      scheduler stalls; Telegram pushes only at vibration burst
      boundaries and retries failed alert sends.

  Motor nameplate: 230 V AC, 50 Hz, 0.55 kW, 4.1 A rated, 2850 RPM.
  THRESHOLD SOURCE OF TRUTH: this configuration block - the physically
  bench-validated limits below are authoritative over documentation.

  --------------------------- LIBRARIES REQUIRED ----------------------
    1. LiquidCrystal_I2C
    2. Adafruit ADXL345_U + Adafruit Unified Sensor
    3. Adafruit ADS1X15
    4. UniversalTelegramBot + ArduinoJson (v6.x)
    5. WebServer + WiFi (built into the ESP32 Arduino core)

  --------------------------- WIRING / PIN MAP -------------------------
    Relay GPIO19, buzzer GPIO23, LEDs GPIO25/33/32, buttons GPIO16/17/18
    (INPUT_PULLUP). I2C on GPIO21/22. ADS1115 at 0x48: A0 voltage,
    A1 current, A2 temperature. ADXL345 on the same I2C bus.
  =====================================================================
*/

#include "secrets.h"   // NEVER committed - see secrets.h.example / .gitignore

#include <Wire.h>
#include <LiquidCrystal_I2C.h>
#include <Adafruit_Sensor.h>
#include <Adafruit_ADXL345_U.h>
#include <Adafruit_ADS1X15.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <WebServer.h>
#include <ArduinoJson.h>
#include <UniversalTelegramBot.h>

#include "event_log.h"
#include "index_html.h"

// =====================================================================
//                 ===== USER CONFIGURATION / CALIBRATION =====
//        BENCH-VALIDATED SOURCE OF TRUTH - change values only here.
// =====================================================================

// ---------------- Telegram pacing ----------------
#define TELEGRAM_STATUS_INTERVAL_MS   10000UL   // periodic "all OK" status push
#define TELEGRAM_REPEAT_ALERT_MS      60000UL   // min spacing between repeat pushes of the SAME active fault
#define TELEGRAM_RETRY_MS             5000UL    // retry cadence for failed alert sends
#define TELEGRAM_LINK_KEEPALIVE_MS    30000UL   // telegram badge stays "linked" this long after last good send

// ---------------- Wireless: AP fallback ----------------
#define ENABLE_AP_FALLBACK 1                    // if STA fails, host a dashboard hotspot instead
#define STA_CONNECT_TIMEOUT_MS 15000UL

// ---------------- Voltage (ZMPT101B, via ADS1115 CH A0) ----------------
double VOLTAGE_CALIBRATION     = 0.19;    // raw ADC-RMS counts * this = Volts RMS
#define LOW_VOLTAGE_LIMIT         180.0    // V  (bench-validated practical limit)
#define HIGH_VOLTAGE_LIMIT        250.0    // V

// ---------------- Current (SCT-013-030, via ADS1115 CH A1) ----------------
double CURRENT_CALIBRATION     = 0.003;   // raw ADC-RMS counts * this = Amps RMS
#define RATED_CURRENT_A            4.1     // nameplate reference - "warn" band on the UI
#define LOW_CURRENT_LIMIT          0.3     // A - "motor not loaded" diagnostics only, not a fault
#define HIGH_CURRENT_LIMIT         6.0     // A - trip point
#define CURRENT_ZERO_DEADBAND      0.05    // A - readings below this are reported as 0.00

// ---------------- Temperature (NTC MF5A-3, B=3950, via ADS1115 CH A2) ----------------
#define NTC_OFFSET_C               0.0
#define AMBIENT_REFERENCE_C        40.0    // Temperature Rise = Winding Temp - this
#define LOW_TEMPERATURE_LIMIT      5.0     // C  - sensor-fault sanity floor
#define HIGH_TEMPERATURE_LIMIT     130.0   // C  - absolute winding ceiling
#define TEMPERATURE_RISE_LIMIT     90.0    // C  - (winding temp - AMBIENT_REFERENCE_C) ceiling

// ---------------- Vibration (ADXL345) ----------------
#define ACCELEROMETER_CALIBRATION  1.0
#define VIBRATION_CALIBRATION      1.0
#define VIBRATION_LOW_LIMIT_MMS    0.71    // mm/s RMS - diagnostics floor
#define VIBRATION_HIGH_LIMIT_MMS   4.50    // mm/s RMS - primary trip point
#define VIBRATION_BACKUP_MG        800.0   // mg RMS - independent backup trip
#define VIB_STARTUP_GRACE_MS       3000UL  // ignore vibration faults just after start

// ---------------- Timing / sampling ----------------
#define LCD_UPDATE_INTERVAL_MS     500UL
#define SENSOR_READ_INTERVAL_MS    1000UL
#define RMS_SAMPLE_COUNT           96      // ~200 ms window at 475 SPS (~10 mains cycles)
#define FAULT_CONFIRM_SAMPLES      3       // consecutive bad readings required before tripping
#define STARTUP_DELAY_MS           5000UL
#define BUTTON_DEBOUNCE_MS         250UL

// =====================================================================
//                    ===== END OF CONFIGURATION =====
// =====================================================================

// ===================== PIN DEFINITIONS =====================
#define NTC_NOMINAL_RES   10000
#define NTC_NOMINAL_TEMP  25
#define NTC_BETA          3950
#define FIXED_RESISTOR    10000
#define ADC_MAX_VALUE     32767
#define RELAY_PIN        19
#define BUZZER_PIN       23
#define LED_RED_PIN      25
#define LED_YELLOW_PIN   33
#define LED_GREEN_PIN    32
#define BTN_RESET_PIN    16
#define BTN_MOTOR_ON_PIN 17
#define BTN_MOTOR_OFF_PIN 18

#define RELAY_ON_STATE  LOW
#define RELAY_OFF_STATE HIGH

// ===================== VIBRATION SAMPLER TIMING =====================
#define VIB_ODR                 ADXL345_DATARATE_400_HZ
#define VIB_SAMPLE_DT_MS         2         // 500 Hz polling (oversamples the 400 Hz ODR)
#define VIB_SAMPLES              500       // 500 samples * 2 ms = 1.0 s RMS burst
#define VIB_HPF_ALPHA            0.7745f   // ~10 Hz cutoff high-pass for 2 ms dt
#define VIB_STALL_GUARD_MS       6         // gap > this = scheduler stall, drop sample pair

// ===================== OBJECTS =====================
LiquidCrystal_I2C lcd(0x27, 16, 4);
Adafruit_ADXL345_Unified accel = Adafruit_ADXL345_Unified(12345);
Adafruit_ADS1115 ads;
#define ADS_CH_VOLTAGE 0
#define ADS_CH_CURRENT 1
#define ADS_CH_TEMPERATURE 2

WiFiClientSecure secured_client;
UniversalTelegramBot bot(BOT_TOKEN, secured_client);
WebServer server(80);

// ===================== STATE VARIABLES =====================
float temperature = 0;        // winding temperature, C
float temperatureRise = 0;    // temperature - AMBIENT_REFERENCE_C
float voltage = 0;
float current = 0;
float vibrationMG = 0;        // backup metric
float vibrationMMS = 0;       // primary metric per spec

bool motorOn = false;
bool faultLatched = false;
String faultText = "";

bool starting = false;
unsigned long startingBeginMillis = 0;
unsigned long motorOnMillis = 0;

unsigned long lastSensorReadMillis = 0;
unsigned long lastTelegramMillis = 0;
unsigned long lastFaultTelegramMillis = 0;
unsigned long lastLCDUpdateMillis = 0;
unsigned long lastButtonCheckMillis = 0;
unsigned long lastResetPress = 0, lastMotorOnPress = 0, lastMotorOffPress = 0;

// fault debounce counters (hysteresis - FAULT_CONFIRM_SAMPLES consecutive
// bad readings are required before tripping)
int overVoltCount = 0, underVoltCount = 0, overCurrCount = 0;
int overTempCount = 0, tempRiseCount = 0, vibCount = 0;

// ---- Non-blocking vibration sampler state ----
unsigned long lastVibSampleMillis = 0;
int vibSampleIndex = 0;
bool vibInitialized = false;
float vibGX = 0, vibGY = 0, vibGZ = 0;
float vibVelX = 0, vibVelY = 0, vibVelZ = 0;
float vibSumSqVel = 0;
float vibSumSqMG = 0;

// ---- Telegram delivery state ----
String pendingTelegramMsg = "";
bool telegramPending = false;
unsigned long lastTelegramRetryMillis = 0;
unsigned long telegramLastOkMillis = 0;
bool telegramLinked = false;

// ---- Wireless state ----
bool wifiInSTA = true;
String deviceIp = "";

// ---- Rollover-safe uptime accumulation ----
uint32_t uptimeMark = 0;
uint32_t uptimeSeconds = 0;

// ---- LCD anti-flicker: only rewrite lines whose text changed ----
String lcdLine[4] = {"", "", "", ""};

// =====================================================================
//                  SHARED ACTUATION HELPERS (SAFETY INTERLOCK)
// Every actuation request (physical buttons AND web commands) flows
// through these three helpers - single source of truth. Requests that
// violate an interlock are silently ignored here; HTTP callers get
// precise 409 diagnostics from their endpoints, buttons simply no-op.
// =====================================================================
void requestMotorStart(const char* source) {
  if (faultLatched || motorOn || starting) return;   // latched fault blocks ON
  starting = true;
  startingBeginMillis = millis();
  eventLog.add(EV_MOTOR_START, "Soft-start initiated (%s)", source);
}

void requestMotorStop(const char* source) {
  if (!motorOn && !starting) return;
  motorOn = false;
  starting = false;
  digitalWrite(RELAY_PIN, RELAY_OFF_STATE);
  eventLog.add(EV_MOTOR_OFF, "Motor OFF (%s)", source);
}

void requestFaultReset(const char* source) {
  if (!faultLatched) return;
  eventLog.add(EV_RESET, "Fault latch cleared (%s) - was: %s", source, faultText.c_str());
  faultLatched = false;
  faultText = "";
  overVoltCount = underVoltCount = overCurrCount = 0;
  overTempCount = tempRiseCount = vibCount = 0;
}

// =====================================================================
void setup() {
  Serial.begin(115200);

  eventLog.begin();
  eventLog.add(EV_BOOT, "Boot - web-dashboard revision");

  pinMode(RELAY_PIN, OUTPUT);
  digitalWrite(RELAY_PIN, RELAY_OFF_STATE);

  pinMode(BUZZER_PIN, OUTPUT);
  digitalWrite(BUZZER_PIN, LOW);

  pinMode(LED_RED_PIN, OUTPUT);
  pinMode(LED_YELLOW_PIN, OUTPUT);
  pinMode(LED_GREEN_PIN, OUTPUT);
  digitalWrite(LED_RED_PIN, HIGH);
  digitalWrite(LED_YELLOW_PIN, LOW);
  digitalWrite(LED_GREEN_PIN, LOW);

  pinMode(BTN_RESET_PIN, INPUT_PULLUP);
  pinMode(BTN_MOTOR_ON_PIN, INPUT_PULLUP);
  pinMode(BTN_MOTOR_OFF_PIN, INPUT_PULLUP);

  Wire.begin(21, 22);
  Wire.setClock(400000); // fast-mode I2C for fast vibration polling

  lcd.init();
  lcd.backlight();
  lcd.clear();
  lcd.setCursor(0, 0);
  lcd.print("Booting system..");
  delay(800);

  if (!accel.begin()) {
    Serial.println("ADXL345 not found - check wiring!");
  }
  accel.setRange(ADXL345_RANGE_2_G);
  accel.setDataRate(VIB_ODR);

  ads.begin(0x48);
  ads.setGain(GAIN_ONE);
  ads.setDataRate(RATE_ADS1115_475SPS);

  connectWiFi();
  secured_client.setInsecure();
  startWebServer();

  lcd.clear();
  for (int i = 0; i < 4; i++) lcdLine[i] = "";
  eventLog.add(EV_INFO, "WebUI active, IP %s", deviceIp.c_str());
}

// =====================================================================
// Cooperative non-blocking loop. Priority order matters:
//   1. HTTP server  (fast idle pass, cheap when no traffic)
//   2. Buttons      (latency-critical human input)
//   3. Vibration    (2 ms pacing, stall-tolerant)
//   4. Sensors/trip (1 s cadence, ~400 ms burst inside)
//   5. LCD / Telegram / uptime (slow cadence, relégable)
// =====================================================================
void loop() {
  server.handleClient();
  handleButtons();
  updateVibrationSampler();

  if (millis() - lastSensorReadMillis >= SENSOR_READ_INTERVAL_MS) {
    lastSensorReadMillis = millis();
    readAllSensors();
    checkFaults();
  }

  handleStartupDelay();
  updateOutputs();

  if (millis() - lastLCDUpdateMillis >= LCD_UPDATE_INTERVAL_MS) {
    lastLCDUpdateMillis = millis();
    updateLCD();
  }

  telegramRetryTick();
  telegramStatusTick();
  uptimeTick();
  telegramLinkageTick();
}

// =====================================================================
void connectWiFi() {
  lcd.clear();
  lcd.setCursor(0, 0);
  lcd.print("Connecting WiFi ");
  Serial.print("\nConnecting WiFi");

  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  unsigned long start = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - start < STA_CONNECT_TIMEOUT_MS) {
    delay(300);
    Serial.print(".");
  }

  if (WiFi.status() == WL_CONNECTED) {
    wifiInSTA = true;
    deviceIp = WiFi.localIP().toString();
    String apName = WiFi.SSID();
    Serial.println("\nWiFi connected, IP: " + deviceIp);
    eventLog.add(EV_WIFI_OK, "STA linked '%s' IP %s", apName.c_str(), deviceIp.c_str());
    return;
  }

  Serial.println("\nWiFi FAILED");
  eventLog.add(EV_WIFI_FAIL, "STA link failed");
#if ENABLE_AP_FALLBACK
  // Dashboard fallback: host our own hotspot so the web UI and local
  // control stay usable (Telegram will be offline until STA returns).
  WiFi.mode(WIFI_AP);
  WiFi.softAP(AP_SSID, AP_PASS);
  delay(300);
  wifiInSTA = false;
  deviceIp = WiFi.softAPIP().toString();
  Serial.println("AP fallback: SSID " + String(AP_SSID) + " IP " + deviceIp);
  eventLog.add(EV_AP_ON, "Fallback AP '%s' IP %s", AP_SSID, deviceIp.c_str());
#endif
}

// =====================================================================
//                     HTTP SERVER + REST API
// =====================================================================
void sendCors() {
  server.sendHeader("Access-Control-Allow-Origin", "*");
  server.sendHeader("Access-Control-Allow-Methods", "GET, POST, OPTIONS");
  server.sendHeader("Access-Control-Allow-Headers", "Content-Type");
}

void handleOptions() {
  sendCors();
  server.send(204, "application/json", "{}");
}

void handleNotFound() {
  sendCors();
  server.send(404, "application/json", "{\"ok\":false,\"message\":\"Unknown endpoint\"}");
}

void handleRoot() {
  sendCors();
  server.send_P(200, "text/html", INDEX_HTML);
}

void handleStatusJson() {
  sendCors();

  DynamicJsonDocument doc(4096);
  doc["up"] = uptimeSeconds;
  doc["heap"] = (uint32_t)ESP.getFreeHeap();

  doc["mOn"] = motorOn;
  doc["st"] = starting;
  if (starting) {
    long rem = (long)STARTUP_DELAY_MS - (long)(millis() - startingBeginMillis);
    doc["stRem"] = rem > 0 ? (int)rem : 0;
  }
  doc["fl"] = faultLatched;
  doc["fault"] = faultLatched ? faultText : String("");

  JsonObject w = doc.createNestedObject("wifi");
  w["mode"] = wifiInSTA ? "STA" : "AP";
  w["ip"] = deviceIp;
  w["rssi"] = wifiInSTA ? (int)WiFi.RSSI() : 0;

  JsonObject tg = doc.createNestedObject("tg");
  tg["ok"] = telegramLinked;

  JsonObject m = doc.createNestedObject("m");
  m["t"] = (double)temperature;
  m["tRise"] = (double)temperatureRise;
  m["i"] = (double)current;
  m["v"] = (double)voltage;
  m["vib"] = (double)vibrationMMS;
  m["vibMg"] = (double)vibrationMG;

  JsonObject lim = doc.createNestedObject("lim");
  lim["t"] = (double)HIGH_TEMPERATURE_LIMIT;
  lim["tRise"] = (double)TEMPERATURE_RISE_LIMIT;
  lim["i"] = (double)HIGH_CURRENT_LIMIT;
  lim["iPlate"] = (double)RATED_CURRENT_A;
  lim["vLow"] = (double)LOW_VOLTAGE_LIMIT;
  lim["vHigh"] = (double)HIGH_VOLTAGE_LIMIT;
  lim["vib"] = (double)VIBRATION_HIGH_LIMIT_MMS;

  // newest-first event window for the dashboard table
  JsonArray ev = doc.createNestedArray("events");
  uint8_t shown = 0;
  for (int8_t i = (int8_t)eventLog.count() - 1; i >= 0 && shown < 12; i--, shown++) {
    const LogEvent* e = eventLog.at((uint8_t)i);
    if (!e) break;
    JsonObject o = ev.createNestedObject();
    o["sq"] = e->seq;
    o["up"] = e->uptime;
    o["tp"] = logTypeLabel(e->type);
    o["tx"] = e->text;
  }

  String out;
  out.reserve(1600);
  serializeJson(doc, out);
  server.send(200, "application/json", out);
}

void handleMotorCmd() {
  sendCors();
  String body = server.arg("plain");
  String action = body;
  action.trim();
  action.toLowerCase();

  if (body.startsWith("{")) {
    DynamicJsonDocument d(192);
    if (deserializeJson(d, body) == DeserializationError::Ok) {
      action = String(d["action"] | "");
      action.trim();
      action.toLowerCase();
    }
  }

  if (action == "on") {
    if (faultLatched) {
      server.send(409, "application/json",
                  "{\"ok\":false,\"message\":\"Fault latched - use Reset Fault first\"}");
      return;
    }
    if (motorOn || starting) {
      server.send(409, "application/json",
                  "{\"ok\":false,\"message\":\"Motor already running or starting\"}");
      return;
    }
    requestMotorStart("WEB");
    server.send(202, "application/json",
                "{\"ok\":true,\"message\":\"Soft-start initiated (5 s interlock)\"}");
    return;
  }

  if (action == "off") {
    if (!motorOn && !starting) {
      server.send(409, "application/json",
                  "{\"ok\":false,\"message\":\"Motor already OFF\"}");
      return;
    }
    requestMotorStop("WEB");
    server.send(200, "application/json",
                "{\"ok\":true,\"message\":\"Motor OFF\"}");
    return;
  }

  server.send(400, "application/json",
              "{\"ok\":false,\"message\":\"Unknown action (use on/off)\"}");
}

void handleReset() {
  sendCors();
  if (!faultLatched) {
    server.send(409, "application/json",
                "{\"ok\":false,\"message\":\"No active fault to reset\"}");
    return;
  }
  requestFaultReset("WEB");
  server.send(200, "application/json",
              "{\"ok\":true,\"message\":\"Fault latch cleared\"}");
}

void handleLog() {
  sendCors();
  if (server.arg("fmt") == "html") {
    handleLogReport();
  } else {
    handleLogCsv();
  }
}

void handleLogCsv() {
  server.sendHeader("Content-Disposition", "attachment; filename=\"motor_log.csv\"");
  String csv = "seq,uptime_ms,uptime,type,event\n";
  for (uint8_t i = 0; i < eventLog.count(); i++) {
    const LogEvent* e = eventLog.at(i);
    csv += String(e->seq) + "," + String(e->uptime) + "," + uptimeToString(e->uptime) +
           "," + logTypeLabel(e->type) + ",\"" + String(e->text) + "\"\n";
  }
  server.send(200, "text/csv", csv);
}

static String htmlEscape(const String& in) {
  String out;
  out.reserve(in.length() + 8);
  for (unsigned int i = 0; i < in.length(); i++) {
    char c = in.charAt(i);
    switch (c) {
      case '&':  out += "&amp;";  break;
      case '<':  out += "&lt;";   break;
      case '>':  out += "&gt;";   break;
      case '"':  out += "&quot;"; break;
      case '\'': out += "&#39;";  break;
      default:   out += c;
    }
  }
  return out;
}

// Printable report: user opens it in a tab and Ctrl+P -> "Save as PDF".
// @media print hides controls; everything is self-contained ink-safe.
void handleLogReport() {
  String rows;
  rows.reserve(eventLog.count() * 96);
  for (uint8_t i = 0; i < eventLog.count(); i++) {
    const LogEvent* e = eventLog.at(i);
    rows += "<tr><td>#";
    rows += String(e->seq);
    rows += "</td><td>";
    rows += uptimeToString(e->uptime);
    rows += "</td><td><b>";
    rows += logTypeLabel(e->type);
    rows += "</b></td><td>";
    rows += htmlEscape(String(e->text));
    rows += "</td></tr>";
  }

  String page;
  page.reserve(2600 + rows.length());
  page += "<!DOCTYPE html><html><head><meta charset='utf-8'>";
  page += "<title>Motor Monitor - Event Log Report</title><style>";
  page += "body{font-family:ui-monospace,Consolas,monospace;color:#0b0f19;margin:24px}";
  page += "h1{font-size:18px;letter-spacing:1px}p{color:#555;font-size:12px}";
  page += "table{width:100%;border-collapse:collapse;font-size:12px;margin-top:14px}";
  page += "th,td{border:1px solid #888;padding:6px 10px;text-align:left}";
  page += "th{background:#e2e8f0}";
  page += "button{padding:8px 14px;margin-top:8px;cursor:pointer}";
  page += "@media print{button{display:none}";
  page += "body{margin:0;color:#000}th,td{border-color:#000}}";
  page += "</style></head><body>";
  page += "<h1>SMART INDUCTION MOTOR MONITOR - EVENT LOG REPORT</h1>";
  page += "<p>Printer-friendly report. Use the button below (or Ctrl+P) and";
  page += " choose 'Save as PDF'. Generated from the in-memory event ring buffer.</p>";
  page += "<button onclick='window.print()'>PRINT / SAVE AS PDF</button>";
  page += "<table><thead><tr><th>SEQ</th><th>UPTIME</th><th>TYPE</th><th>EVENT</th>";
  page += "</tr></thead><tbody>";
  page += rows;
  page += "</tbody></table></body></html>";

  server.send(200, "text/html", page);
}

void startWebServer() {
  server.on("/", HTTP_GET, handleRoot);
  server.on("/api/status", HTTP_GET, handleStatusJson);
  server.on("/api/motor", HTTP_POST, handleMotorCmd);
  server.on("/api/motor", HTTP_OPTIONS, handleOptions);
  server.on("/api/reset", HTTP_POST, handleReset);
  server.on("/api/reset", HTTP_OPTIONS, handleOptions);
  server.on("/api/log", HTTP_GET, handleLog);
  server.onNotFound(handleNotFound);
  server.begin();
  Serial.println("WebUI active on port 80 at " + deviceIp);
}

// =====================================================================
void uptimeTick() {
  // 1 Hz rollover-safe accumulation of uptimeSeconds
  if (millis() - uptimeMark >= 1000UL) {
    uptimeMark += 1000UL;
    uptimeSeconds++;
  }
}

// =====================================================================
// NTC via ADS1115 A2. Beta-equation, with NTC_OFFSET_C trim applied.
float readNTCTemperature() {
  const int SAMPLES = 10;
  long adcSum = 0;
  for (int i = 0; i < SAMPLES; i++) {
    int16_t reading = ads.readADC_SingleEnded(ADS_CH_TEMPERATURE);
    if (reading < 0) reading = 0;
    adcSum += reading;
  }
  float adcAvg = (float)adcSum / SAMPLES;

  if (adcAvg <= 0 || adcAvg >= ADC_MAX_VALUE - 1) {
    return -999; // disconnected / shorted - invalid, ignored by caller
  }

  float resistance = FIXED_RESISTOR * (adcAvg / ((float)ADC_MAX_VALUE - adcAvg));

  float steinhart = resistance / NTC_NOMINAL_RES;
  steinhart = log(steinhart);
  steinhart /= NTC_BETA;
  steinhart += 1.0 / (NTC_NOMINAL_TEMP + 273.15);
  steinhart = 1.0 / steinhart;
  steinhart -= 273.15;

  return steinhart;
}

// =====================================================================
// Non-blocking vibration sampler - one accel sample per call. Tolerates
// scheduler stalls (Telegram TLS, HTTP bursts): if the wall-clock gap
// between two consecutive samples is too large, that sample pair is
// skipped so a stall can't inject a false velocity spike into the RMS.
void updateVibrationSampler() {
  unsigned long now = millis();
  unsigned long dtMs = now - lastVibSampleMillis;
  if (dtMs < VIB_SAMPLE_DT_MS) return;
  lastVibSampleMillis = now;
  if (dtMs > VIB_STALL_GUARD_MS) return;   // stall guard: drop this sample

  sensors_event_t event;
  accel.getEvent(&event);
  float ax = event.acceleration.x * ACCELEROMETER_CALIBRATION;
  float ay = event.acceleration.y * ACCELEROMETER_CALIBRATION;
  float az = event.acceleration.z * ACCELEROMETER_CALIBRATION;

  if (!vibInitialized) { vibGX = ax; vibGY = ay; vibGZ = az; vibInitialized = true; }

  vibGX = VIB_HPF_ALPHA * vibGX + (1 - VIB_HPF_ALPHA) * ax;
  vibGY = VIB_HPF_ALPHA * vibGY + (1 - VIB_HPF_ALPHA) * ay;
  vibGZ = VIB_HPF_ALPHA * vibGZ + (1 - VIB_HPF_ALPHA) * az;

  float axAC = ax - vibGX;
  float ayAC = ay - vibGY;
  float azAC = az - vibGZ;

  const float dt = VIB_SAMPLE_DT_MS / 1000.0f;

  // Backup metric: mg RMS directly from filtered acceleration (no
  // integration, can't drift - sanity check vs the mm/s figure).
  float accMagMS2 = sqrt(axAC * axAC + ayAC * ayAC + azAC * azAC);
  float accMagMG = (accMagMS2 / 9.80665f) * 1000.0f;
  vibSumSqMG += accMagMG * accMagMG;

  // Primary metric: integrate filtered acceleration to velocity, then
  // high-pass the velocity so it can't wander from a DC offset.
  vibVelX = (vibVelX + axAC * dt) * VIB_HPF_ALPHA;
  vibVelY = (vibVelY + ayAC * dt) * VIB_HPF_ALPHA;
  vibVelZ = (vibVelZ + azAC * dt) * VIB_HPF_ALPHA;

  float velMag = sqrt(vibVelX * vibVelX + vibVelY * vibVelY + vibVelZ * vibVelZ);
  vibSumSqVel += velMag * velMag;
  vibSampleIndex++;

  if (vibSampleIndex >= VIB_SAMPLES) {
    vibrationMG = sqrt(vibSumSqMG / VIB_SAMPLES);
    float velRMS_ms = sqrt(vibSumSqVel / VIB_SAMPLES);
    vibrationMMS = velRMS_ms * 1000.0f * VIBRATION_CALIBRATION;

    vibSampleIndex = 0;
    vibSumSqVel = 0;
    vibSumSqMG = 0;
  }
}

// =====================================================================
void readAllSensors() {
  float t = readNTCTemperature();
  if (t > -50 && t < 200) {
    temperature = t + NTC_OFFSET_C;
    temperatureRise = temperature - AMBIENT_REFERENCE_C;
  }

  float rawV = readRMS(ADS_CH_VOLTAGE) * VOLTAGE_CALIBRATION;
  float rawI = readRMS(ADS_CH_CURRENT) * CURRENT_CALIBRATION;

  voltage = rawV;
  current = (rawI < CURRENT_ZERO_DEADBAND) ? 0.0 : rawI;
}

// RMS of AC component of raw ADC counts (DC bias removed by mean subtraction).
// ~200 ms window at a fixed 475 SPS data rate. Blocking by design - it
// delays the HTTP response slightly but cannot corrupt trip timing.
double readRMS(uint8_t channel) {
  const int N = RMS_SAMPLE_COUNT;
  static int16_t samples[RMS_SAMPLE_COUNT];
  long sum = 0;

  for (int i = 0; i < N; i++) {
    samples[i] = ads.readADC_SingleEnded(channel);
    sum += samples[i];
  }
  double mean = (double)sum / N;

  double sumSq = 0;
  for (int i = 0; i < N; i++) {
    double diff = samples[i] - mean;
    sumSq += diff * diff;
  }
  return sqrt(sumSq / N);
}

// =====================================================================
void checkFaults() {
  if (faultLatched) return;

  bool vibrationGraceActive = motorOn && (millis() - motorOnMillis < VIB_STARTUP_GRACE_MS);

  // Evaluate each condition independently, then require
  // FAULT_CONFIRM_SAMPLES consecutive bad readings to trip.
  overVoltCount   = (voltage > HIGH_VOLTAGE_LIMIT) ? overVoltCount + 1 : 0;
  underVoltCount  = (voltage < LOW_VOLTAGE_LIMIT && voltage > 20) ? underVoltCount + 1 : 0;
  overCurrCount   = (current > HIGH_CURRENT_LIMIT) ? overCurrCount + 1 : 0;
  overTempCount   = (temperature > HIGH_TEMPERATURE_LIMIT) ? overTempCount + 1 : 0;
  tempRiseCount   = (temperatureRise > TEMPERATURE_RISE_LIMIT) ? tempRiseCount + 1 : 0;
  vibCount        = (!vibrationGraceActive &&
                      (vibrationMMS > VIBRATION_HIGH_LIMIT_MMS || vibrationMG > VIBRATION_BACKUP_MG))
                      ? vibCount + 1 : 0;

  String newFault = "";
  if (overTempCount   >= FAULT_CONFIRM_SAMPLES) newFault = "HIGH TEMPERATURE";
  else if (tempRiseCount >= FAULT_CONFIRM_SAMPLES) newFault = "EXCESSIVE TEMPERATURE RISE";
  else if (overVoltCount  >= FAULT_CONFIRM_SAMPLES) newFault = "OVER VOLTAGE";
  else if (underVoltCount >= FAULT_CONFIRM_SAMPLES) newFault = "UNDER VOLTAGE";
  else if (overCurrCount  >= FAULT_CONFIRM_SAMPLES) newFault = "OVER CURRENT";
  else if (vibCount       >= FAULT_CONFIRM_SAMPLES) newFault = "HIGH VIBRATION";

  if (newFault != "") {
    faultLatched = true;
    faultText = newFault;
    motorOn = false;
    starting = false;
    digitalWrite(RELAY_PIN, RELAY_OFF_STATE);

    eventLog.add(EV_TRIP, "%s - motor tripped OFF", newFault.c_str());

    // queue the Telegram alert (kept pending if the TLS send fails)
    String alert = buildFaultAlertMessage();
    if (!sendTelegramMessage(alert)) {
      pendingTelegramMsg = alert;
      telegramPending = true;
      lastTelegramRetryMillis = millis();
    }
    lastFaultTelegramMillis = millis();
    lastTelegramMillis = millis();
  }
}

// =====================================================================
void handleButtons() {
  unsigned long now = millis();

  if (digitalRead(BTN_RESET_PIN) == LOW && now - lastResetPress > BUTTON_DEBOUNCE_MS) {
    lastResetPress = now;
    requestFaultReset("BUTTON");
  }

  if (digitalRead(BTN_MOTOR_ON_PIN) == LOW && now - lastMotorOnPress > BUTTON_DEBOUNCE_MS) {
    lastMotorOnPress = now;
    requestMotorStart("BUTTON");
  }

  if (digitalRead(BTN_MOTOR_OFF_PIN) == LOW && now - lastMotorOffPress > BUTTON_DEBOUNCE_MS) {
    lastMotorOffPress = now;
    requestMotorStop("BUTTON");
  }
}

// =====================================================================
void handleStartupDelay() {
  if (!starting) return;
  if (faultLatched) {
    starting = false;
    return;
  }
  if (millis() - startingBeginMillis >= STARTUP_DELAY_MS) {
    starting = false;
    motorOn = true;
    motorOnMillis = millis();
    digitalWrite(RELAY_PIN, RELAY_ON_STATE);

    eventLog.add(EV_MOTOR_ON, "Relay energized - motor running");

    // fresh vibration filter state for the new burst after start
    vibInitialized = false;
    vibVelX = 0; vibVelY = 0; vibVelZ = 0;
    vibSumSqVel = 0;
    vibSumSqMG = 0;
    vibSampleIndex = 0;
  }
}

// =====================================================================
void updateOutputs() {
  digitalWrite(BUZZER_PIN, faultLatched ? HIGH : LOW);
  digitalWrite(LED_YELLOW_PIN, faultLatched ? HIGH : LOW);
  digitalWrite(LED_RED_PIN, (!motorOn && !starting) ? HIGH : LOW);
  digitalWrite(LED_GREEN_PIN, (motorOn && !faultLatched) ? HIGH : LOW);
}

// =====================================================================
// Anti-flicker LCD: never calls lcd.clear() in normal operation; each
// of the 4 lines is only rewritten when its text actually changes and
// every string is padded to 16 columns.
void lcdPrintLine(uint8_t row, String text) {
  while (text.length() < 16) text += ' ';
  if (text.length() > 16) text = text.substring(0, 16);
  if (text != lcdLine[row]) {
    lcdLine[row] = text;
    lcd.setCursor(0, row);
    lcd.print(text);
  }
}

void updateLCD() {
  if (faultLatched) {
    lcdPrintLine(0, "*** FAULT ***");
    lcdPrintLine(1, faultText);
    lcdPrintLine(2, "MOTOR: OFF");
    lcdPrintLine(3, "Press RESET btn");
    return;
  }

  if (starting) {
    unsigned long remaining = STARTUP_DELAY_MS - (millis() - startingBeginMillis);
    lcdPrintLine(0, "Starting......");
    lcdPrintLine(1, "Wait " + String((remaining / 1000) + 1) + "s");
    lcdPrintLine(2, "");
    lcdPrintLine(3, "");
    return;
  }

  char line0[17], line1[17];
  snprintf(line0, sizeof(line0), "T:%.1fC V:%.0fV", temperature, voltage);
  snprintf(line1, sizeof(line1), "I:%.2fA Vb:%.2f", current, vibrationMMS);

  lcdPrintLine(0, String(line0));
  lcdPrintLine(1, String(line1));
  lcdPrintLine(2, motorOn ? "Motor: ON" : "Motor: OFF");
  lcdPrintLine(3, motorOn ? "Status: RUNNING" : "Status: STOPPED");
}

// =====================================================================
//                        TELEGRAM DELIVERY
// =====================================================================
bool sendTelegramMessage(const String& msg) {
  if (WiFi.status() != WL_CONNECTED) return false;
  bool ok = bot.sendMessage(CHAT_ID, msg, "");
  if (ok) {
    telegramLastOkMillis = millis();
  }
  return ok;
}

String buildFaultAlertMessage() {
  String msg = "!!! FAULT ALERT !!!\n";
  msg += "Fault: " + faultText + "\n";
  msg += "Motor has been turned OFF automatically.\n";
  msg += "Voltage: " + String(voltage, 0) + " V RMS\n";
  msg += "Current: " + String(current, 2) + " A RMS\n";
  msg += "Temperature Rise: " + String(temperatureRise, 0) +
         " C (winding " + String(temperature, 1) + " C)\n";
  msg += "Vibration: " + String(vibrationMG, 0) + " mg RMS / " +
         String(vibrationMMS, 2) + " mm/s RMS\n";
  msg += "Clear the fault via WebUI / RESET button, then MOTOR ON.";
  return msg;
}

String buildStatusMessage() {
  String msg = "Motor Status Update\n";
  msg += "Motor: "; msg += (motorOn ? "ON" : "OFF"); msg += "\n";
  msg += "Voltage: " + String(voltage, 0) + " V RMS\n";
  msg += "Current: " + String(current, 2) + " A RMS\n";
  msg += "Temperature Rise: " + String(temperatureRise, 0) +
         " C (winding " + String(temperature, 1) + " C)\n";
  msg += "Vibration: " + String(vibrationMG, 0) + " mg RMS / " +
         String(vibrationMMS, 2) + " mm/s RMS\n";
  if (faultLatched) {
    msg += "FAULT: " + faultText + "\n";
  } else if (starting) {
    msg += "Status: Soft-starting\n";
  } else {
    msg += "Status: Normal\n";
  }
  return msg;
}

// Retry a failed fault alert at a fixed cadence. Runs only at a
// vibration burst boundary so a 1-2 s TLS stall can't corrupt the
// in-flight velocity integration.
void telegramRetryTick() {
  if (!telegramPending) return;
  if (millis() - lastTelegramRetryMillis < TELEGRAM_RETRY_MS) return;
  lastTelegramRetryMillis = millis();
  if (WiFi.status() != WL_CONNECTED) return;
  if (vibSampleIndex != 0) return;   // burst boundary guard
  if (sendTelegramMessage(pendingTelegramMsg)) {
    telegramPending = false;
  }
}

// Periodic status push, with the "don't spam the same active fault
// faster than TELEGRAM_REPEAT_ALERT_MS" rule preserved.
void telegramStatusTick() {
  if (faultLatched && (millis() - lastFaultTelegramMillis < TELEGRAM_REPEAT_ALERT_MS)) return;
  if (millis() - lastTelegramMillis < TELEGRAM_STATUS_INTERVAL_MS) return;
  if (vibSampleIndex != 0) return;   // burst boundary guard

  lastTelegramMillis = millis();
  bool ok = sendTelegramMessage(buildStatusMessage());
  if (ok && faultLatched) {
    lastFaultTelegramMillis = millis();
  }
}

// Dashboard "Telegram linked" badge: keep it lit for a grace window
// after the last successful send, then degrade to OFFLINE.
void telegramLinkageTick() {
  telegramLinked = (WiFi.status() == WL_CONNECTED) &&
                   (millis() - telegramLastOkMillis < TELEGRAM_LINK_KEEPALIVE_MS);
}
