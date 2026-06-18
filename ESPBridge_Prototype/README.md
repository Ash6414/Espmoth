# ESPBridge Prototype

This is a clean ESP32 bring-up sketch for the AudioMoth ESPBridge. It is deliberately small: no Wi-Fi, no server API, no upload manifests, and no delete flow. Use it to prove the bridge pins and UART before building the full node firmware back up.

## Hardware

- ESP32 `GPIO32 RX2` <- AudioMoth `b9 UART TX`
- ESP32 `GPIO33 TX2` -> AudioMoth `b10 UART RX`
- ESP32 `GPIO25` -> AudioMoth `a7 ESP_REQ`
- ESP32 `GPIO26` <- AudioMoth `a8 MOTH_BUSY`
- Common ground

## AudioMoth Setup

- Flash an AudioMoth ESPBridge build from `Ash6414/AudioMoth-Firmware_ESPnode` PR #2.
- Put the AudioMoth switch in `CUSTOM`.
- Disable GPS time setting, because `a7/a8` are used for bridge handshake.
- Bridge UART baud is `9600` on both sides. The ESP32 USB serial monitor still runs at `115200`.

The AudioMoth firmware must both report `AudioMoth-Firmware-Basic` for Configurator compatibility and include the ESPBridge service-window fix. An older binary can have the Basic name and bridge strings but still fail this probe if it does not enter `ESPBridge_serviceUntil()` when `MOTH_BUSY` drops.

## Serial Commands

Open the ESP32 serial monitor at `115200`.

- `open`: assert `ESP_REQ`, wait for `MOTH_BUSY` low, then wait for `OK BRIDGE_READY` or `OK PONG`
- `ping`: send `PING`
- `status`: send `STATUS`
- `list`: send `LIST` and print `FILE` lines
- `time <epoch>`: send `TIME <epoch> 0`
- `done`: send `DONE` and deassert `ESP_REQ`
- `pins`: print REQ/BUSY/UART settings
- `raw <command>`: send any raw bridge command
- `reqprobe <seconds>`: hold `ESP_REQ` high, send repeated `PING`, and log `REQ`, `BUSY`, and every UART line
- `rxdiag <seconds>`: capture raw UART RX edge timing and try common baud-rate decodes
- `watch <seconds>`: log `REQ`, `BUSY`, and incoming UART without changing pins

By default the sketch runs one boot probe equivalent to `reqprobe 30`.

## Bring-Up Sequence

1. Flash this sketch to the ESP32.
2. Open serial monitor at `115200`.
3. Put AudioMoth in `CUSTOM`.
4. Reset the ESP32, or run `reqprobe 30` manually.

Expected signs of life:

- `BUSY` changes from `1` to `0` after `REQ` goes high.
- AudioMoth sends `OK BRIDGE_READY` or `OK PONG`.
- The probe ends with `RESULT: PASS basic ESP32 <-> AudioMoth bridge communication detected.`
- If `BUSY=0` but no `OK BRIDGE_READY` or `OK PONG`, the AudioMoth firmware is not entering the bridge service loop.
- If `UART_BYTES` rises but `UART lines received` stays `0`, the ESP32 is seeing bytes that are not valid newline-terminated bridge text. That usually points to a baud/path/pin-level problem or a floating UART RX line.
- If raw GPIO timing decodes bridge text but hardware UART gets zero bytes, check that no code calls `pinMode()` on GPIO32/GPIO33 after `Serial2.begin(...)`; doing that can detach RX2 from the ESP32 pin matrix.
