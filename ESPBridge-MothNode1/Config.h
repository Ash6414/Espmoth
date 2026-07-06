#pragma once

/*
  Moth_Node_ESPBridge configuration

  This file intentionally ships without per-node credentials. The first-boot
  setup portal saves Wi-Fi, server, and HMAC values into ESP32 NVS flash.
*/

// ----------------------------
// First-flash defaults. Leave these blank for fleet deployment builds.
// The ESP32 stores real values in NVS flash after the setup portal saves them.
// ----------------------------
#define DEFAULT_WIFI_SSID       ""
#define DEFAULT_WIFI_PASSWORD   ""
#define DEFAULT_WIFI_SECURITY   "personal"

#define DEFAULT_BASE_URL        "https://cerberus.tail1881ff.ts.net"
#define SERVER_FALLBACK_BASE_URL "http://192.168.0.207:8001"
#define PREFER_FALLBACK_SERVER_WHEN_REACHABLE true
#define FALLBACK_SERVER_PROBE_TIMEOUT_MS 2500
#define DEFAULT_NODE_ID         ""
#define DEFAULT_KEY_ID          ""
#define DEFAULT_DEVICE_SECRET   ""

// First-boot setup portal. If required credentials are missing from NVS, the
// ESP32 starts this local Wi-Fi AP and serves http://192.168.4.1.
#define PROVISION_AP_PREFIX     "BatNode"
#define PROVISION_AP_PASSWORD   "batnode-setup"
#define PROVISION_PORTAL_TIMEOUT_MS  0UL
#define WIFI_RECOVERY_PORTAL_TIMEOUT_MS  600000UL
#define PROVISION_FORCE_PIN     -1

// server endpoints.
#define ENDPOINT_SERVER_TIME      "/v1/public/server_time"
#define ENDPOINT_HEARTBEAT        "/v1/device/heartbeat"
#define ENDPOINT_TIME_CHECK       "/v1/device/time_check"

// Current MothServer upload lifecycle.
// The ESP posts a manifest, creates an upload session per wanted file, PUTs
// AudioMoth chunks, completes the upload, and then asks for delete authorization.
#define ENDPOINT_FILES_MANIFEST   "/v1/files/manifest"
#define ENDPOINT_UPLOAD_INIT      "/v1/uploads/init"

// ----------------------------
// ESP32-WROOM-U pins
// ----------------------------
#define PIN_BATTERY_ADC           34
#define PIN_CHRG                  39
#define PIN_DONE                  36

// AudioMoth firmware uart bridge wiring.
#define PIN_MOTH_UART_RX          32   // ESP32 RX2  <- AudioMoth b9 UART TX
#define PIN_MOTH_UART_TX          33   // ESP32 TX2  -> AudioMoth b10 UART RX
#define PIN_MOTH_REQ              25   // ESP32 out  -> AudioMoth a7 ESP_REQ
#define PIN_MOTH_BUSY             26   // ESP32 in   <- AudioMoth a8 MOTH_BUSY

// ----------------------------
// Battery calibration
// ----------------------------
#define BATTERY_DIVIDER_RATIO     2.0f
#define BATTERY_CAL_FACTOR        0.738f
#define BATTERY_SETTLE_READS      4
#define BATTERY_SAMPLE_COUNT      24
#define BATTERY_SAMPLE_DELAY_MS   2

// conservative thresholds for 1S Li-ion pack
#define MIN_WIFI_BATTERY_V        3.50f
#define MIN_UPLOAD_BATTERY_V      3.60f
#define MIN_MOTH_TIME_SYNC_V      3.40f
#define MIN_ACTIVE_BATTERY_V      3.25f
#define BATTERY_SENSE_INVALID_BELOW_V  1.00f

// Require charging/DONE for automatic WAV uploads. Command UPLOAD_NOW bypasses
// the setup-time upload threshold; the in-transfer cutoff still protects a
// valid low battery reading.
#define REQUIRE_CHARGING_FOR_AUTO_UPLOAD  false

// ----------------------------
// Timing
// ----------------------------
#define DEFAULT_SLEEP_MINUTES        15
#define LOW_BATTERY_SLEEP_MINUTES    60
#define WIFI_CONNECT_TIMEOUT_MS      15000
#define HTTP_TIMEOUT_MS              20000
#define NTP_SYNC_TIMEOUT_MS          15000
#define ENROLLMENT_POLL_INTERVAL_MS  3000

// ----------------------------
// AudioMoth bridge protocol
// ----------------------------
#define MOTH_UART_BAUD               115200
// Production path: one 115200 GETPIPE command keeps the AudioMoth SD file open
// and sends repeated 115200-baud blocks. Each UART frame is ACKed after CRC
// validation; the ESP sends NEXT only after the server accepts the previous
// block.
#define MOTH_PIPE_ENABLED            1
#define MOTH_ALLOW_115200_GET_FALLBACK 0
#define MOTH_PIPE_BAUD               115200
#define MOTH_PIPE_FRAME_RETRIES      3
#define MOTH_STREAM_FRAME_TIMEOUT_MS 4000
#define MOTH_UART_RX_BUFFER_BYTES    8192
#define MOTH_READY_TIMEOUT_MS        65000
#define MOTH_LINE_TIMEOUT_MS         6000
#define MOTH_LIST_TIMEOUT_MS         60000
#define MOTH_DATA_HEADER_TIMEOUT_MS  6000
#define MOTH_BINARY_TIMEOUT_MS       8000
#define MOTH_BUSY_WAIT_MS            5000
#define MOTH_ASSERT_REQ_AT_BOOT      true
#define MOTH_MAX_FILES_PER_SESSION   16
#define MOTH_CHUNK_BYTES             2048
#define SERVER_UPLOAD_CHUNK_BYTES    65536
#define MOTH_UPLOAD_WINDOW_WAIT_MS   120000
#define MOTH_UPLOAD_WINDOW_RETRY_MS  5000
#define USB_BRIDGE_DEBUG_ON_SERVER_FAIL true
#define USB_BRIDGE_DEBUG_WINDOW_MS   120000

// Delete on AudioMoth only after server confirms full file upload.
#define DELETE_AFTER_CONFIRMED_UPLOAD true

// ----------------------------
// Debug
// ----------------------------
#define SERIAL_BAUD                  115200
#define DEBUG_BRIDGE_LINES           false
#define DEBUG_HTTP_RESPONSES         false
