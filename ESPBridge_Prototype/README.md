# ESPBridge Prototype

This is a clean ESP32 bring-up sketch for the AudioMoth ESPBridge. It is deliberately small: no Wi-Fi, no server API, no upload manifests, and no delete flow. Use it to prove the bridge pins and UART before building the full node firmware back up.

## Hardware

- ESP32 `GPIO16 RX2` <- AudioMoth `b9 UART TX`
- ESP32 `GPIO17 TX2` -> AudioMoth `b10 UART RX`
- ESP32 `GPIO25` -> AudioMoth `a7 ESP_REQ`
- ESP32 `GPIO26` <- AudioMoth `a8 MOTH_BUSY`
- Common ground

## AudioMoth Setup

- Flash the AudioMoth ESPBridge firmware.
- Put the AudioMoth switch in `CUSTOM`.
- Disable GPS time setting, because `a7/a8` are used for bridge handshake.
- Baud is `115200` on both sides.

The AudioMoth firmware only calls `ESPBridge_serviceUntil()` from the Custom-mode safe service window. If the ESP sees `BUSY=0` but never receives `OK BRIDGE_READY` or `OK PONG`, first check the switch is in `CUSTOM`.

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
- `watch <seconds>`: log `REQ`, `BUSY`, and incoming UART without changing pins

By default the sketch runs one boot probe: `open`, `PING`, `STATUS`, `DONE`.

## Bring-Up Sequence

1. Flash this sketch to the ESP32.
2. Open serial monitor at `115200`.
3. Put AudioMoth in `CUSTOM`.
4. Run `reqprobe 30`.

Expected signs of life:

- `BUSY` changes from `1` to `0` after `REQ` goes high.
- AudioMoth sends `OK BRIDGE_READY` or `OK PONG`.
- If `BUSY=0` but no `OK BRIDGE_READY` or `OK PONG`, the AudioMoth firmware is not entering the bridge service loop.
