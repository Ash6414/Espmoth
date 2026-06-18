#pragma once

// ESP32 serial monitor.
#define SERIAL_BAUD                 115200

// AudioMoth ESPBridge UART.
#define MOTH_UART_BAUD              9600
#define PIN_MOTH_UART_RX            32   // ESP32 RX2 <- AudioMoth b9 UART TX
#define PIN_MOTH_UART_TX            33   // ESP32 TX2 -> AudioMoth b10 UART RX

// AudioMoth bridge handshake.
#define PIN_MOTH_REQ                25   // ESP32 output -> AudioMoth a7 ESP_REQ
#define PIN_MOTH_BUSY               26   // ESP32 input  <- AudioMoth a8 MOTH_BUSY

// Timing.
#define MOTH_BUSY_WAIT_MS           30000
#define MOTH_READY_WAIT_MS          20000
#define MOTH_LINE_WAIT_MS           4000
#define READY_PROBE_INTERVAL_MS     1500

// Raw GPIO timing capture for AudioMoth TX diagnostics.
#define RAW_RX_CAPTURE_EDGES        2048

// Set to 1 if you want the prototype to run the REQ/UART probe once at boot.
#define AUTO_PROBE_ON_BOOT          1
#define AUTO_REQ_PROBE_SECONDS      30
