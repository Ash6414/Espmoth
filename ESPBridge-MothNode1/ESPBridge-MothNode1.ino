/*
  ESP32-WROOM-U firmware for AudioMoth Dev custom ESP bridge firmware.

  Arduino libraries:
    - ArduinoJson by Benoit Blanchon
    - ESP32 Arduino core

  Wiring:
    ESP32 GPIO32 RX2  <- AudioMoth b9 UART TX
    ESP32 GPIO33 TX2  -> AudioMoth b10 UART RX
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
#include <WiFiClientSecure.h>
#include <ArduinoJson.h>
#include <time.h>
#include <sys/time.h>
#include "mbedtls/md.h"
#include "mbedtls/sha256.h"
#include "driver/rtc_io.h"
#include "esp_mac.h"

#include "Config.h"
#include "TlsConfig.h"
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
uint8_t *serverChunk = nullptr;
String lastUploadMessage = "boot";
UploadSummary lastUpload = {UPLOAD_NOT_ATTEMPTED, 0, 0, 0, "not attempted"};
PowerState preWifiPowerState = {0.0f, 0.0f, false, false};
bool preWifiPowerValid = false;

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
String pathWithoutQuery(const String &pathAndQuery);
bool connectWiFi();
bool syncClockFromNtp();
bool beginHttpClient(HTTPClient &http, WiFiClient &plainClient, WiFiClientSecure &secureClient, const String &url);
void syncSystemClock(uint32_t epochUtc);
uint32_t estimatedEpochUtc();
long getServerTime(uint32_t *rttMsOut);
void addAuthHeaders(HTTPClient &http, const String &method, const String &path, const uint8_t *body, size_t bodyLen, long serverEpoch);
bool signedPostJson(const String &path, const String &body, long serverEpoch, String &responseOut);
bool signedGet(const String &path, long serverEpoch, String &responseOut);
bool signedPostBinary(const String &pathAndQuery, const uint8_t *body, size_t bodyLen, long serverEpoch, String &responseOut);
bool signedPutBinary(const String &pathAndQuery, const uint8_t *body, size_t bodyLen, long serverEpoch, String &responseOut);

// Provisioning.ino
bool loadNodeConfig();
bool nodeConfigReady();
bool provisioningForced();
void requestProvisioningOnNextBoot();
void runProvisioningPortal(bool recoveryMode = false);
const String &cfgWifiSsid();
const String &cfgWifiPassword();
const String &cfgWifiSecurity();
const String &cfgWifiIdentity();
const String &cfgWifiUsername();
bool beginWiFiConnection(const String &ssid, const String &securityMode, const String &identity, const String &username, const String &password);
const String &cfgBaseUrl();
const String &cfgNodeId();
const String &cfgKeyId();
const String &cfgDeviceSecret();

// AudioMothBridge.ino
void initMothBridge();
bool mothBusy();
void mothRequest(bool state);
bool bridgeWaitReady(uint32_t timeoutMs);
bool bridgePing();
bool bridgeEnableFastBaud();
uint32_t bridgeTransferChunkBytes();
bool bridgeSetTime(uint32_t epochUtc, uint32_t milliseconds);
bool bridgeStatus(String &statusOut);
bool bridgeList(MothFile *files, size_t maxFiles, size_t &countOut, MothSdInfo *sdInfo);
bool bridgeGetChunk(const String &path, uint32_t offset, uint32_t maxBytes, ChunkResult &result);
bool bridgeDelete(const String &path);
void bridgeDone();
void bridgeRestoreDefaultBaud();
uint32_t crc32Update(uint32_t crc, const uint8_t *data, uint32_t length);

// ServerApi.ino
bool postHeartbeat(long serverEpoch, const PowerState &p, const UploadSummary &upload);
bool postTimeCheck(long serverEpoch, uint32_t rttMs, const String &notes);
void ackCommand(long serverEpoch, int commandId, const String &msg);
void pollCommands(long serverEpoch, const PowerState &p);
String serverManifestId();
String serverFilenameFromPath(const String &path);
uint32_t serverLocalFileId(const MothFile &file);
bool serverPostManifest(long serverEpoch, MothFile *files, size_t fileCount, const MothSdInfo &sdInfo, String &manifestIdOut);
bool serverInitFile(long serverEpoch, const String &manifestId, const MothFile &file, UploadSession &session);
bool serverUploadChunk(long serverEpoch, const UploadSession &session, const uint8_t *data, uint32_t offset, uint32_t length, uint32_t &serverProcessMs);
bool serverFinishFile(long serverEpoch, const UploadSession &session);
bool serverFetchDeleteAuthorization(long serverEpoch, const String &manifestId, MothFile *files, size_t fileCount, DeleteCandidate *candidates, size_t maxCandidates, size_t &candidateCount, String &authorizationId);
bool serverConfirmDeletes(long serverEpoch, const String &authorizationId, DeleteCandidate *candidates, size_t candidateCount);

// Upload.ino
bool openBridgeSession(long serverEpoch);
void closeBridgeSession();
UploadSummary runAudioMothUploadSession(long serverEpoch, bool forced);
bool uploadOneFile(long serverEpoch, const String &manifestId, const MothFile &file, bool &bridgeFailure);
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

  bool haveConfig = loadNodeConfig();
  if (!haveConfig || provisioningForced()) {
    runProvisioningPortal(false);
  }

  initPowerPins();
  initMothBridge();

  esp_sleep_wakeup_cause_t wakeCause = esp_sleep_get_wakeup_cause();

  Serial.println();
  Serial.println("=== ESP32 AudioMoth bridge node ===");
  Serial.printf("Node ID: %s\n", cfgNodeId().c_str());
  Serial.printf("Server: %s\n", cfgBaseUrl().c_str());
  Serial.printf("Boot count: %lu\n", (unsigned long)rtcBootCounter);
  Serial.printf("Wake cause: %d\n", (int)wakeCause);
  Serial.printf("MOTH_BUSY=%d\n", mothBusy());
  Serial.printf("ESP_REQ=%d\n", digitalRead(PIN_MOTH_REQ));

  Serial.println("Sampling resting battery before Wi-Fi...");
  PowerState power = readPowerState();
  preWifiPowerState = power;
  preWifiPowerValid = true;
  Serial.printf("Pre-Wi-Fi battery: %.3f V, %.1f%%, CHRG=%d, DONE=%d\n",
                power.batteryV, power.batteryPercent, power.charging, power.chargeDone);
  if (power.batteryV < BATTERY_SENSE_INVALID_BELOW_V) {
    Serial.println("Battery sense is absent/invalid; allowing Wi-Fi setup but disabling file upload.");
  }

  if (!powerAllowsWiFi(power)) {
    lastUpload = {UPLOAD_SKIPPED_POWER, 0, 0, 0, "battery below Wi-Fi threshold"};
    deepSleepMinutes(LOW_BATTERY_SLEEP_MINUTES);
  }

  uint32_t rttMs = 0;
  long serverEpoch = 0;
  if (!fetchFreshServerTimeAndSync(&rttMs, &serverEpoch)) {
    lastUpload = {UPLOAD_NOT_ATTEMPTED, 0, 0, 0, "server time unavailable"};
    if (WiFi.status() != WL_CONNECTED && nodeConfigReady()) {
      Serial.println("Saved Wi-Fi is unavailable; opening the recovery portal.");
      runProvisioningPortal(true);
    }
    deepSleepMinutes(DEFAULT_SLEEP_MINUTES);
  }

  postTimeCheck(serverEpoch, rttMs, "ESP32 clock synced from server; AudioMoth time will be set over UART bridge when service window opens");
  postHeartbeat(serverEpoch, power, lastUpload);
  pollCommands(serverEpoch, power);

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
