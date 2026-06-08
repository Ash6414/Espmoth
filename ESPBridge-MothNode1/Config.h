#pragma once

/*
  Moth_Node_ESPBridge configuration

  Paste your current Wi-Fi and HMAC settings from the old working sketch here.
  This file intentionally ships with placeholders.
*/

// ----------------------------
// Wi-Fi and server
// ----------------------------
#define WIFI_SSID       "Willows"
#define WIFI_PASSWORD   "Willows0824!!"

#define BASE_URL        "http://192.168.0.207:8000"
#define NODE_ID         "BATNODE_001"
#define KEY_ID          "key-1"
#define DEVICE_SECRET   "065ea008379ad8ac28c79563181f88087539657c2cce77d447e41eb9b64a5873"

// Existing server endpoints from the working GPS-spoof sketch.
#define ENDPOINT_SERVER_TIME      "/v1/public/server_time"
#define ENDPOINT_HEARTBEAT        "/v1/device/heartbeat"
#define ENDPOINT_TIME_CHECK       "/v1/device/time_check"

// New upload endpoints. Your FastAPI server must implement these before upload will succeed.
// The ESP will never DELETE on the AudioMoth unless all upload calls return 2xx.
#define ENDPOINT_UPLOAD_START     "/v1/device/" NODE_ID "/upload/start"
#define ENDPOINT_UPLOAD_CHUNK     "/v1/device/" NODE_ID "/upload/chunk"
#define ENDPOINT_UPLOAD_FINISH    "/v1/device/" NODE_ID "/upload/finish"

// ----------------------------
// ESP32-WROOM-U pins
// ----------------------------
#define PIN_BATTERY_ADC           34
#define PIN_CHRG                  39
#define PIN_DONE                  36

// New custom AudioMoth firmware bridge wiring.
// This replaces the old GPS-spoof use of a7/a8.
#define PIN_MOTH_UART_RX          16   // ESP32 RX2  <- AudioMoth b9 UART TX
#define PIN_MOTH_UART_TX          17   // ESP32 TX2  -> AudioMoth b10 UART RX
#define PIN_MOTH_REQ              25   // ESP32 out  -> AudioMoth a7 ESP_REQ
#define PIN_MOTH_BUSY             26   // ESP32 in   <- AudioMoth a8 MOTH_BUSY

// ----------------------------
// Battery calibration
// ----------------------------
#define BATTERY_DIVIDER_RATIO     2.0f
#define BATTERY_CAL_FACTOR        0.738f

// conservative thresholds for 1S Li-ion pack
#define MIN_WIFI_BATTERY_V        3.50f
#define MIN_UPLOAD_BATTERY_V      3.75f
#define MIN_MOTH_TIME_SYNC_V      3.40f

// Require charging/DONE for automatic WAV uploads. Command UPLOAD_NOW bypasses this but not MIN_UPLOAD_BATTERY_V.
#define REQUIRE_CHARGING_FOR_AUTO_UPLOAD  true

// ----------------------------
// Timing
// ----------------------------
#define DEFAULT_SLEEP_MINUTES        15
#define LOW_BATTERY_SLEEP_MINUTES    60
#define WIFI_CONNECT_TIMEOUT_MS      15000
#define HTTP_TIMEOUT_MS              20000

// ----------------------------
// AudioMoth bridge protocol
// ----------------------------
#define MOTH_UART_BAUD               921600
#define MOTH_READY_TIMEOUT_MS        12000
#define MOTH_LINE_TIMEOUT_MS         4000
#define MOTH_BUSY_WAIT_MS            3000
#define MOTH_MAX_FILES_PER_SESSION   16
#define MOTH_CHUNK_BYTES             512

// Delete on AudioMoth only after server confirms full file upload.
#define DELETE_AFTER_CONFIRMED_UPLOAD true

// ----------------------------
// Debug
// ----------------------------
#define SERIAL_BAUD                  115200
#define DEBUG_BRIDGE_LINES           true
#define DEBUG_HTTP_RESPONSES         true
