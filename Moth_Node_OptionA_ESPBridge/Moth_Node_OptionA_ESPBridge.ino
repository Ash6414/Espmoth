/*
  Moth Node Option A - Dedicated ESPBridge

  This sketch intentionally removes GPS spoofing.

  AudioMoth Dev pins:
    b9  AudioMoth UART TX  -> ESP32 GPIO16 RX2
    b10 AudioMoth UART RX  <- ESP32 GPIO17 TX2
    a7  ESP_REQ input      <- ESP32 GPIO25 output
    a8  MOTH_BUSY output   -> ESP32 GPIO26 input
    GND common

  Server contract:
    GET  /v1/public/server_time
    POST /v1/device/heartbeat
    POST /v1/device/time_check
    GET  /v1/device/{node_id}/commands
    POST /v1/device/{node_id}/commands/{command_id}/ack

  AudioMoth bridge commands used here:
    PING
    STATUS
    TIME <unix_seconds> <milliseconds>
    DONE

  Upload commands are deliberately left out of this first stable Option A sketch.
  Get time sync stable first, then add LIST/GET/upload/delete on top.
*/

#include <Arduino.h>
#include <WiFi.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include "mbedtls/md.h"

// ----------------------------
// User configuration
// ----------------------------
static const char *WIFI_SSID = "YOUR_WIFI_SSID";
static const char *WIFI_PASSWORD = "YOUR_WIFI_PASSWORD";

static const char *SERVER_BASE_URL = "http://192.168.1.100:8000";

static const char *NODE_ID = "BATNODE_001";
static const char *KEY_ID = "key-1";
static const char *DEVICE_SECRET = "REPLACE_WITH_64_HEX_OR_SERVER_SECRET";

// ----------------------------
// ESP32 <-> AudioMoth bridge pins
// ----------------------------
static constexpr int PIN_MOTH_UART_RX = 16;  // ESP32 RX2  <- AudioMoth b9 TX
static constexpr int PIN_MOTH_UART_TX = 17;  // ESP32 TX2  -> AudioMoth b10 RX
static constexpr int PIN_MOTH_REQ     = 25;  // ESP32 OUT  -> AudioMoth a7 ESP_REQ
static constexpr int PIN_MOTH_BUSY    = 26;  // ESP32 IN   <- AudioMoth a8 MOTH_BUSY

// Solar controller / battery pins. Adjust only if your wiring changes.
static constexpr int PIN_BATTERY_ADC = 34;
static constexpr int PIN_CHRG        = 39;
static constexpr int PIN_DONE        = 36;

static constexpr float ADC_REFERENCE_V = 3.3f;
static constexpr float ADC_MAX_COUNTS = 4095.0f;
static constexpr float BATTERY_DIVIDER_RATIO = 2.0f;
static constexpr float BATTERY_CAL_FACTOR = 0.739f;

static constexpr uint32_t SERIAL_BAUD = 115200;
static constexpr uint32_t MOTH_UART_BAUD = 921600;
static constexpr uint32_t WIFI_TIMEOUT_MS = 20000;
static constexpr uint32_t MOTH_BUSY_WAIT_MS = 65000;
static constexpr uint32_t MOTH_READY_TIMEOUT_MS = 20000;
static constexpr uint32_t MOTH_LINE_TIMEOUT_MS = 5000;
static constexpr uint32_t WAKE_INTERVAL_SECONDS = 15 * 60;

HardwareSerial MothSerial(2);

static bool bridgeOpen = false;
static uint32_t espEpoch = 0;
static uint32_t espEpochMillis = 0;

// ----------------------------
// Basic time helpers
// ----------------------------
uint32_t currentEpoch() {
  if (espEpoch == 0) return 0;
  return espEpoch + ((millis() - espEpochMillis) / 1000UL);
}

void setEspEpoch(uint32_t epoch) {
  espEpoch = epoch;
  espEpochMillis = millis();
}

String hexSha256(const uint8_t *data, size_t len) {
  uint8_t hash[32];
  mbedtls_md_context_t ctx;
  mbedtls_md_init(&ctx);
  const mbedtls_md_info_t *info = mbedtls_md_info_from_type(MBEDTLS_MD_SHA256);
  mbedtls_md_setup(&ctx, info, 0);
  mbedtls_md_starts(&ctx);
  mbedtls_md_update(&ctx, data, len);
  mbedtls_md_finish(&ctx, hash);
  mbedtls_md_free(&ctx);

  char out[65];
  for (int i = 0; i < 32; i++) sprintf(out + i * 2, "%02x", hash[i]);
  out[64] = 0;
  return String(out);
}

String hmacSha256Hex(const String &message, const char *secret) {
  uint8_t mac[32];
  mbedtls_md_context_t ctx;
  mbedtls_md_init(&ctx);
  const mbedtls_md_info_t *info = mbedtls_md_info_from_type(MBEDTLS_MD_SHA256);
  mbedtls_md_setup(&ctx, info, 1);
  mbedtls_md_hmac_starts(&ctx, reinterpret_cast<const uint8_t *>(secret), strlen(secret));
  mbedtls_md_hmac_update(&ctx, reinterpret_cast<const uint8_t *>(message.c_str()), message.length());
  mbedtls_md_hmac_finish(&ctx, mac);
  mbedtls_md_free(&ctx);

  char out[65];
  for (int i = 0; i < 32; i++) sprintf(out + i * 2, "%02x", mac[i]);
  out[64] = 0;
  return String(out);
}

String randomNonce() {
  char out[17];
  uint32_t a = esp_random();
  uint32_t b = esp_random();
  snprintf(out, sizeof(out), "%08lx%08lx", static_cast<unsigned long>(a), static_cast<unsigned long>(b));
  return String(out);
}

// ----------------------------
// Power telemetry
// ----------------------------
float readBatteryVoltage() {
  uint32_t raw = analogRead(PIN_BATTERY_ADC);
  float adcV = (static_cast<float>(raw) / ADC_MAX_COUNTS) * ADC_REFERENCE_V;
  return adcV * BATTERY_DIVIDER_RATIO * BATTERY_CAL_FACTOR;
}

float batteryPercentFromVoltage(float v) {
  if (v <= 3.30f) return 0.0f;
  if (v >= 4.20f) return 100.0f;
  return (v - 3.30f) * 100.0f / (4.20f - 3.30f);
}

bool isCharging() {
  return digitalRead(PIN_CHRG) == HIGH;
}

bool isChargeDone() {
  return digitalRead(PIN_DONE) == HIGH;
}

// ----------------------------
// Wi-Fi and signed HTTP
// ----------------------------
bool connectWiFi() {
  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);

  Serial.print("WiFi connecting");
  uint32_t start = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - start < WIFI_TIMEOUT_MS) {
    Serial.print(".");
    delay(250);
  }
  Serial.println();

  if (WiFi.status() == WL_CONNECTED) {
    Serial.print("WiFi OK, IP=");
    Serial.println(WiFi.localIP());
    return true;
  }

  Serial.println("WiFi failed.");
  return false;
}

bool getPublicServerTime(uint32_t &serverEpochOut) {
  HTTPClient http;
  String url = String(SERVER_BASE_URL) + "/v1/public/server_time";
  http.begin(url);
  int code = http.GET();
  if (code != 200) {
    Serial.printf("server_time HTTP %d\n", code);
    http.end();
    return false;
  }

  String response = http.getString();
  http.end();

  StaticJsonDocument<256> doc;
  DeserializationError err = deserializeJson(doc, response);
  if (err) return false;

  serverEpochOut = doc["epoch_utc"] | 0;
  return serverEpochOut > 0;
}

bool signedRequest(const char *method, const String &pathWithQuery, const String &body, int &statusOut, String &responseOut, bool includeQueryInSignature) {
  uint32_t now = currentEpoch();
  if (now == 0) return false;

  String bodyHash = hexSha256(reinterpret_cast<const uint8_t *>(body.c_str()), body.length());
  String timestamp = String(now);
  String nonce = randomNonce();

  String canonicalPath = pathWithQuery;
  if (!includeQueryInSignature) {
    int q = canonicalPath.indexOf('?');
    if (q >= 0) canonicalPath = canonicalPath.substring(0, q);
  }

  String canonical = String(method) + "\n" + canonicalPath + "\n" + timestamp + "\n" + nonce + "\n" + bodyHash;
  String signature = hmacSha256Hex(canonical, DEVICE_SECRET);

  HTTPClient http;
  String url = String(SERVER_BASE_URL) + pathWithQuery;
  http.begin(url);
  http.addHeader("X-Node-ID", NODE_ID);
  http.addHeader("X-Key-ID", KEY_ID);
  http.addHeader("X-Timestamp", timestamp);
  http.addHeader("X-Nonce", nonce);
  http.addHeader("X-Body-SHA256", bodyHash);
  http.addHeader("X-Signature", signature);
  http.addHeader("Content-Type", "application/json");

  if (strcmp(method, "GET") == 0) {
    statusOut = http.GET();
  } else if (strcmp(method, "POST") == 0) {
    statusOut = http.POST(reinterpret_cast<const uint8_t *>(body.c_str()), body.length());
  } else {
    http.end();
    return false;
  }

  responseOut = http.getString();
  http.end();

  Serial.printf("%s %s -> %d\n", method, pathWithQuery.c_str(), statusOut);
  if (responseOut.length()) Serial.println(responseOut);

  return statusOut >= 200 && statusOut < 300;
}

bool postHeartbeat(const char *mode, const char *uploadStatus, const String &bridgeStatus) {
  StaticJsonDocument<768> doc;
  doc["node_id"] = NODE_ID;
  float battery = readBatteryVoltage();
  doc["battery_v"] = battery;
  doc["battery_percent"] = batteryPercentFromVoltage(battery);
  doc["charging"] = isCharging();
  doc["charge_done"] = isChargeDone();
  doc["wifi_rssi_dbm"] = WiFi.RSSI();
  doc["recording_status"] = mothBusy() ? "MOTH_BUSY" : "MOTH_IDLE";
  doc["upload_status"] = uploadStatus;
  doc["mode"] = mode;
  if (bridgeStatus.length()) doc["bridge"]["status"] = bridgeStatus;

  String body;
  serializeJson(doc, body);

  int status = 0;
  String response;
  return signedRequest("POST", "/v1/device/heartbeat", body, status, response, false);
}

bool postTimeCheck(uint32_t serverEpoch, uint32_t espBefore, uint32_t espAfter, uint32_t mothEpoch, uint32_t rttMs, const char *notes) {
  StaticJsonDocument<512> doc;
  doc["node_id"] = NODE_ID;
  doc["server_epoch"] = serverEpoch;
  doc["esp_epoch_before"] = espBefore;
  doc["esp_epoch_after"] = espAfter;
  doc["audiomoth_epoch"] = mothEpoch;
  doc["rtt_ms"] = rttMs;
  doc["time_source"] = "server_time_then_espbridge_time";
  doc["notes"] = notes;

  String body;
  serializeJson(doc, body);

  int status = 0;
  String response;
  return signedRequest("POST", "/v1/device/time_check", body, status, response, false);
}

bool getCommands(JsonDocument &docOut) {
  int status = 0;
  String response;
  String path = String("/v1/device/") + NODE_ID + "/commands";
  if (!signedRequest("GET", path, "", status, response, false)) return false;

  DeserializationError err = deserializeJson(docOut, response);
  return !err;
}

bool ackCommand(int commandId, bool ok, const String &message) {
  StaticJsonDocument<256> doc;
  doc["response"]["ok"] = ok;
  doc["response"]["message"] = message;

  String body;
  serializeJson(doc, body);

  int status = 0;
  String response;
  String path = String("/v1/device/") + NODE_ID + "/commands/" + String(commandId) + "/ack";
  return signedRequest("POST", path, body, status, response, false);
}

// ----------------------------
// AudioMoth bridge
// ----------------------------
bool mothBusy() {
  return digitalRead(PIN_MOTH_BUSY) == HIGH;
}

void mothRequest(bool state) {
  digitalWrite(PIN_MOTH_REQ, state ? HIGH : LOW);
}

void bridgeFlushInput() {
  uint32_t lastByteMs = millis();
  while (millis() - lastByteMs < 25) {
    while (MothSerial.available()) {
      (void)MothSerial.read();
      lastByteMs = millis();
    }
    delay(1);
  }
}

bool isBridgePrintable(char c) {
  return c >= 32 && c <= 126;
}

void bridgeSendLine(const String &line) {
  Serial.print("MOTH << ");
  Serial.println(line);
  MothSerial.print(line);
  MothSerial.print('\n');
  MothSerial.flush();
}

bool bridgeReadLine(String &line, uint32_t timeoutMs) {
  line = "";
  uint32_t start = millis();

  while (millis() - start < timeoutMs) {
    while (MothSerial.available()) {
      char c = static_cast<char>(MothSerial.read());
      if (c == '\r') continue;

      if (c == '\n') {
        if (line.length() == 0) continue;
        Serial.print("MOTH >> ");
        Serial.println(line);
        return true;
      }

      if (!isBridgePrintable(c)) {
        Serial.printf("MOTH >> [discarded non-ASCII byte 0x%02X]\n", static_cast<uint8_t>(c));
        line = "";
        continue;
      }

      if (line.length() < 240) line += c;
    }
    delay(1);
  }

  return false;
}

bool waitForMothIdle(uint32_t waitMs) {
  uint32_t start = millis();
  while (millis() - start < waitMs) {
    if (!mothBusy()) return true;
    delay(50);
  }
  return !mothBusy();
}

bool bridgeWaitReady(uint32_t timeoutMs) {
  String line;
  uint32_t start = millis();
  while (millis() - start < timeoutMs) {
    if (bridgeReadLine(line, 500)) {
      if (line == "OK BRIDGE_READY") return true;
      if (line.startsWith("OK BRIDGE_SLEEP")) continue;
      if (line.startsWith("ERR")) return false;
    }
  }
  return false;
}

bool openBridgeSession() {
  if (bridgeOpen) return true;

  Serial.println("Opening bridge session...");
  Serial.printf("Initial MOTH_BUSY=%d\n", mothBusy() ? 1 : 0);

  // Flush before request only. Do not flush after MOTH_BUSY drops; AudioMoth
  // may already have sent OK BRIDGE_READY.
  bridgeFlushInput();
  mothRequest(true);
  delay(20);

  Serial.printf("ESP_REQ=1, MOTH_BUSY=%d\n", mothBusy() ? 1 : 0);
  if (mothBusy() && !waitForMothIdle(MOTH_BUSY_WAIT_MS)) {
    Serial.println("FAIL: MOTH_BUSY stayed high after ESP_REQ.");
    mothRequest(false);
    bridgeFlushInput();
    return false;
  }

  Serial.println("MOTH_BUSY is low; waiting for OK BRIDGE_READY...");
  if (!bridgeWaitReady(MOTH_READY_TIMEOUT_MS)) {
    Serial.println("FAIL: AudioMoth did not send OK BRIDGE_READY.");
    mothRequest(false);
    bridgeFlushInput();
    return false;
  }

  bridgeOpen = true;
  return true;
}

bool bridgeCommand(const String &cmd, String &response, uint32_t timeoutMs = MOTH_LINE_TIMEOUT_MS) {
  if (!bridgeOpen && !openBridgeSession()) return false;
  bridgeSendLine(cmd);
  return bridgeReadLine(response, timeoutMs);
}

void closeBridgeSession() {
  if (bridgeOpen) {
    String response;
    bridgeSendLine("DONE");
    (void)bridgeReadLine(response, 2000);
  }
  bridgeFlushInput();
  mothRequest(false);
  bridgeOpen = false;
  Serial.println("Bridge closed: ESP_REQ=0");
}

bool bridgePing() {
  String response;
  return bridgeCommand("PING", response) && response.startsWith("OK");
}

bool bridgeSetTime(uint32_t epoch, uint32_t milliseconds) {
  String response;
  String cmd = String("TIME ") + String(epoch) + " " + String(milliseconds);
  bool ok = bridgeCommand(cmd, response) && response.startsWith("OK TIME");
  return ok;
}

bool bridgeStatus(String &statusOut) {
  String response;
  bool ok = bridgeCommand("STATUS", response) && response.startsWith("OK STATUS");
  statusOut = response;
  return ok;
}

uint32_t parseMothEpochFromStatus(const String &status) {
  int idx = status.indexOf("now=");
  if (idx < 0) return 0;
  int start = idx + 4;
  int end = status.indexOf(' ', start);
  String value = (end < 0) ? status.substring(start) : status.substring(start, end);
  return static_cast<uint32_t>(strtoul(value.c_str(), nullptr, 10));
}

bool syncMothTimeFromServer() {
  uint32_t espBefore = currentEpoch();
  uint32_t serverEpoch = 0;
  uint32_t startMs = millis();

  if (!getPublicServerTime(serverEpoch)) {
    Serial.println("Could not get public server time.");
    return false;
  }

  uint32_t rtt = millis() - startMs;
  setEspEpoch(serverEpoch);

  if (!openBridgeSession()) {
    postTimeCheck(serverEpoch, espBefore, currentEpoch(), 0, rtt, "bridge_open_failed");
    return false;
  }

  bool timeOk = bridgeSetTime(serverEpoch, 0);
  String status;
  bool statusOk = bridgeStatus(status);
  uint32_t mothEpoch = statusOk ? parseMothEpochFromStatus(status) : 0;
  closeBridgeSession();

  bool checkOk = postTimeCheck(serverEpoch, espBefore, currentEpoch(), mothEpoch, rtt, timeOk ? "ok" : "TIME_command_failed");
  (void)checkOk;

  return timeOk && statusOk;
}

void handleCommand(int commandId, const String &type) {
  if (type == "PING") {
    ackCommand(commandId, true, "ESP online");
    return;
  }

  if (type == "MOTH_STATUS") {
    String status;
    bool ok = false;
    if (openBridgeSession()) {
      ok = bridgeStatus(status);
      closeBridgeSession();
    }
    ackCommand(commandId, ok, ok ? status : "MOTH_STATUS failed");
    return;
  }

  if (type == "SYNC_MOTH_TIME") {
    bool ok = syncMothTimeFromServer();
    ackCommand(commandId, ok, ok ? "AudioMoth time synced over ESPBridge" : "AudioMoth time sync failed");
    return;
  }

  if (type == "UPLOAD_NOW") {
    ackCommand(commandId, false, "UPLOAD_NOW not enabled in first Option A sketch; time bridge is enabled");
    return;
  }

  ackCommand(commandId, false, "unknown command");
}

void pollAndHandleCommands() {
  StaticJsonDocument<2048> doc;
  if (!getCommands(doc)) {
    Serial.println("No commands or command parse failed.");
    return;
  }

  JsonArray commands = doc["commands"].as<JsonArray>();
  for (JsonObject cmd : commands) {
    int id = cmd["id"] | 0;
    String type = cmd["type"] | "";
    type.toUpperCase();
    if (id > 0) handleCommand(id, type);
  }
}

void goToSleep() {
  Serial.printf("Sleeping for %lu seconds.\n", static_cast<unsigned long>(WAKE_INTERVAL_SECONDS));
  WiFi.disconnect(true);
  WiFi.mode(WIFI_OFF);
  esp_sleep_enable_timer_wakeup(static_cast<uint64_t>(WAKE_INTERVAL_SECONDS) * 1000000ULL);
  esp_deep_sleep_start();
}

void setup() {
  Serial.begin(SERIAL_BAUD);
  delay(500);

  pinMode(PIN_MOTH_REQ, OUTPUT);
  digitalWrite(PIN_MOTH_REQ, LOW);
  pinMode(PIN_MOTH_BUSY, INPUT);

  pinMode(PIN_CHRG, INPUT);
  pinMode(PIN_DONE, INPUT);
  analogReadResolution(12);
  analogSetPinAttenuation(PIN_BATTERY_ADC, ADC_11db);

  MothSerial.begin(MOTH_UART_BAUD, SERIAL_8N1, PIN_MOTH_UART_RX, PIN_MOTH_UART_TX);
  bridgeFlushInput();

  Serial.println();
  Serial.println("Moth Node Option A - ESPBridge only");
  Serial.println("GPS spoofing disabled. Time updates use bridge TIME command.");
  Serial.printf("MOTH_BUSY=%d\n", mothBusy() ? 1 : 0);

  if (!connectWiFi()) {
    goToSleep();
  }

  uint32_t serverEpoch = 0;
  if (getPublicServerTime(serverEpoch)) {
    setEspEpoch(serverEpoch);
  } else {
    Serial.println("Could not get server time. Sleeping.");
    goToSleep();
  }

  String bridgeStatusText;
  if (openBridgeSession()) {
    (void)bridgePing();
    (void)bridgeStatus(bridgeStatusText);
    closeBridgeSession();
  }

  postHeartbeat("awake", "idle", bridgeStatusText);
  pollAndHandleCommands();
  postHeartbeat("sleeping", "idle", "");

  goToSleep();
}

void loop() {
  // Not used; this node is wake-work-sleep.
}
