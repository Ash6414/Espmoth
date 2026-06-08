/*
  ESP32-WROOM-U firmware for AudioMoth Dev custom ESP bridge firmware.

  This sketch replaces the earlier fake-GPS sketch.
  AudioMoth now owns its own SD card. The ESP32 requests files over UART,
  uploads them to the FastAPI server, and asks AudioMoth to delete only after
  confirmed server upload.

  Arduino libraries:
    - ArduinoJson by Benoit Blanchon
    - ESP32 Arduino core

  Wiring:
    ESP32 GPIO16 RX2  <- AudioMoth b9 UART TX
    ESP32 GPIO17 TX2  -> AudioMoth b10 UART RX
    ESP32 GPIO25 OUT  -> AudioMoth a7 ESP_REQ
    ESP32 GPIO26 IN   <- AudioMoth a8 MOTH_BUSY
    ESP32 GPIO34 ADC  <- charge controller battery divider
    ESP32 GPIO39 IN   <- charge controller CHRG
    ESP32 GPIO36 IN   <- charge controller DONE
    GND               -> common GND

  AudioMoth config requirement:
    Disable GPS time setting. a7/a8 are now bridge handshake pins, not GPS pins.
*/

#include <WiFi.h>
#include <HTTPClient.h>
#include <WiFiClient.h>
#include <ArduinoJson.h>
#include <time.h>
#include <sys/time.h>
#include "mbedtls/md.h"
#include "mbedtls/sha256.h"
#include "driver/rtc_io.h"

#include "Config.h"
#include "Types.h"

HardwareSerial MothSerial(2);

RTC_DATA_ATTR uint32_t rtcBootCounter = 0;
RTC_DATA_ATTR uint32_t rtcSuccessfulUploads = 0;
RTC_DATA_ATTR uint32_t rtcFailedUploads = 0;
RTC_DATA_ATTR uint32_t rtcLastServerEpoch = 0;
RTC_DATA_ATTR int64_t rtcLastSyncEspUs = 0;

uint32_t bootServerEpoch = 0;
uint32_t bootServerMillis = 0;
bool bootHasFreshServerTime = false;

uint8_t mothChunk[MOTH_CHUNK_BYTES];
String lastUploadMessage = "boot";
UploadSummary lastUpload = {UPLOAD_NOT_ATTEMPTED, 0, 0, 0, "not attempted"};

// Power.ino
void initPowerPins();
PowerState readPowerState();
float estimateBatteryPercent(float v);
bool powerAllowsWiFi(const PowerState &p);
bool powerAllowsUpload(const PowerState &p, bool forced);

// Sleep.ino
void prepareWakePins();
void deepSleepMinutes(uint32_t minutes);

// AuthHttp.ino
String sha256Hex(const uint8_t *data, size_t len);
String sha256Hex(const String &s);
String hmacSha256Hex(const String &key, const String &message);
String randomNonce();
String urlEncode(const String &s);
bool connectWiFi();
void syncSystemClock(uint32_t epochUtc);
uint32_t estimatedEpochUtc();
long getServerTime(uint32_t *rttMsOut);
void addAuthHeaders(HTTPClient &http, const String &method, const String &path, const uint8_t *body, size_t bodyLen, long serverEpoch);
bool signedPostJson(const String &path, const String &body, long serverEpoch, String &responseOut);
bool signedGet(const String &path, long serverEpoch, String &responseOut);
bool signedPostBinary(const String &pathAndQuery, const uint8_t *body, size_t bodyLen, long serverEpoch, String &responseOut);

// AudioMothBridge.ino
void initMothBridge();
bool mothBusy();
void mothRequest(bool state);
bool bridgeWaitReady(uint32_t timeoutMs);
bool bridgePing();
bool bridgeSetTime(uint32_t epochUtc, uint32_t milliseconds);
bool bridgeStatus(String &statusOut);
bool bridgeList(MothFile *files, size_t maxFiles, size_t &countOut);
bool bridgeGetChunk(const String &path, uint32_t offset, uint32_t maxBytes, ChunkResult &result);
bool bridgeDelete(const String &path);
void bridgeDone();
uint32_t crc32Update(uint32_t crc, const uint8_t *data, uint32_t length);

// ServerApi.ino
bool postHeartbeat(long serverEpoch, const PowerState &p, const UploadSummary &upload);
bool postTimeCheck(long serverEpoch, uint32_t rttMs, const String &notes);
void ackCommand(long serverEpoch, int commandId, const String &msg);
void pollCommands(long serverEpoch, const PowerState &p);
bool serverBeginFile(long serverEpoch, const MothFile &file);
bool serverUploadChunk(long serverEpoch, const MothFile &file, const ChunkResult &chunk);
bool serverFinishFile(long serverEpoch, const MothFile &file);

// Upload.ino
UploadSummary runAudioMothUploadSession(long serverEpoch, bool forced);
bool uploadOneFile(long serverEpoch, const MothFile &file);
bool syncMothTimeOnly(long serverEpoch);

bool fetchFreshServerTimeAndSync(uint32_t *rttMsOut, long *serverEpochOut) {
  if (!connectWiFi()) return false;

  uint32_t rttMs = 0;
  long serverEpoch = getServerTime(&rttMs);

  if (serverEpoch <= 1700000000L) {
    Serial.println("Could not get valid server time.");
    return false;
  }

  syncSystemClock((uint32_t)serverEpoch);
  bootHasFreshServerTime = true;
  bootServerEpoch = (uint32_t)serverEpoch;
  bootServerMillis = millis();
  rtcLastServerEpoch = (uint32_t)serverEpoch;
  rtcLastSyncEspUs = esp_timer_get_time();

  if (rttMsOut) *rttMsOut = rttMs;
  if (serverEpochOut) *serverEpochOut = serverEpoch;

  Serial.printf("Server epoch: %ld, RTT: %lu ms\n", serverEpoch, (unsigned long)rttMs);
  return true;
}

void setup() {
  Serial.begin(SERIAL_BAUD);
  delay(300);

  rtcBootCounter += 1;
  bootHasFreshServerTime = false;

  initPowerPins();
  initMothBridge();

  esp_sleep_wakeup_cause_t wakeCause = esp_sleep_get_wakeup_cause();
  PowerState power = readPowerState();

  Serial.println();
  Serial.println("=== ESP32 AudioMoth bridge node ===");
  Serial.printf("Boot count: %lu\n", (unsigned long)rtcBootCounter);
  Serial.printf("Wake cause: %d\n", (int)wakeCause);
  Serial.printf("Battery: %.3f V, %.1f%%, CHRG=%d, DONE=%d\n",
                power.batteryV, power.batteryPercent, power.charging, power.chargeDone);
  Serial.printf("MOTH_BUSY=%d\n", mothBusy());

  if (!powerAllowsWiFi(power)) {
    lastUpload = {UPLOAD_SKIPPED_POWER, 0, 0, 0, "battery below Wi-Fi threshold"};
    deepSleepMinutes(LOW_BATTERY_SLEEP_MINUTES);
  }

  uint32_t rttMs = 0;
  long serverEpoch = 0;
  if (!fetchFreshServerTimeAndSync(&rttMs, &serverEpoch)) {
    lastUpload = {UPLOAD_NOT_ATTEMPTED, 0, 0, 0, "server time unavailable"};
    deepSleepMinutes(DEFAULT_SLEEP_MINUTES);
  }

  postTimeCheck(serverEpoch, rttMs, "ESP32 clock synced from server; AudioMoth time will be set over UART bridge when service window opens");
  postHeartbeat(serverEpoch, power, lastUpload);
  pollCommands(serverEpoch, power);

  power = readPowerState();
  if (powerAllowsUpload(power, false)) {
    lastUpload = runAudioMothUploadSession(serverEpoch, false);
    if (lastUpload.code == UPLOAD_SUCCESS) rtcSuccessfulUploads += 1;
    if (lastUpload.code == UPLOAD_SERVER_FAILED || lastUpload.code == UPLOAD_BRIDGE_FAILED) rtcFailedUploads += 1;
    postHeartbeat(serverEpoch, power, lastUpload);
  } else {
    lastUpload = {UPLOAD_SKIPPED_POWER, 0, 0, 0, "auto upload skipped by power policy"};
    postHeartbeat(serverEpoch, power, lastUpload);
  }

  deepSleepMinutes(DEFAULT_SLEEP_MINUTES);
}

void loop() {
  // Deep sleep controls normal scheduling.
}
