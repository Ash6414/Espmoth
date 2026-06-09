# Moth_Node_ESPBridge

ESP32-WROOM-U Arduino firmware for the custom AudioMoth Dev ESP bridge firmware.

## What this sketch does

- Connects to Wi-Fi only during wake windows.
- Fetches signed server time from `/v1/public/server_time`.
- Sends HMAC-signed heartbeat and time-check messages.
- Polls queued commands.
- Reads battery voltage on GPIO34.
- Reads charge controller CHRG on GPIO39 and DONE on GPIO36.
- Talks to AudioMoth over UART at 9600 baud.
- Requests AudioMoth file service using ESP_REQ on GPIO25 -> AudioMoth a7.
- Respects AudioMoth busy state on GPIO26 <- AudioMoth a8.
- Lists WAV files, fetches them in CRC-checked chunks, uploads chunks to server, and deletes from AudioMoth only after full server confirmation.

## Wiring

```text
ESP32-WROOM-U                 AudioMoth Dev
GPIO16 RX2  <---------------- b9  UART TX
GPIO17 TX2  ----------------> b10 UART RX
GPIO25 OUT  ----------------> a7  ESP_REQ
GPIO26 IN   <---------------- a8  MOTH_BUSY
GND         ----------------- GND
```

Power/status:

```text
GPIO34 ADC  <- battery divided sense from solar controller
GPIO39 IN   <- CHRG
GPIO36 IN   <- DONE
```

## AudioMoth requirement

Disable GPS time setting in the AudioMoth configuration. This firmware does not use GPS spoofing. Time is sent with:

```text
TIME <unix_seconds> <milliseconds>
```

The ESP32 UART setup must leave GPIO16/GPIO17 owned by `Serial2` after `Serial2.begin(...)`. Do not call `pinMode()` on either UART pin after `begin()`, or the ESP32 pin matrix can detach RX2 and the bridge will see GPIO edges but decode zero UART bytes.

## Arduino setup

1. Open the `Moth_Node_ESPBridge` folder in Arduino IDE.
2. Select your ESP32-WROOM-U board profile.
3. Install `ArduinoJson`.
4. Edit `Config.h`:
   - Wi-Fi SSID/password
   - `BASE_URL`
   - `NODE_ID`
   - `KEY_ID`
   - `DEVICE_SECRET`
5. Upload to the ESP32.

## Server endpoints needed for WAV upload

Existing endpoints reused from your current server:

```text
GET  /v1/public/server_time
POST /v1/device/heartbeat
POST /v1/device/time_check
GET  /v1/device/{NODE_ID}/commands
POST /v1/device/{NODE_ID}/commands/{id}/ack
```

The ESP32 asserts ESP_REQ before waiting for MOTH_BUSY to fall. That order matters because AudioMoth opens the UART bridge only after seeing the request pin.

Upload/delete endpoints this ESP firmware expects from the current MothServer:

```text
POST /v1/files/manifest
POST /v1/uploads/init
PUT  /v1/uploads/{upload_id}/chunks/{chunk_index}
POST /v1/uploads/{upload_id}/complete
GET  /v1/nodes/{NODE_ID}/delete_authorization?manifest_id=...
POST /v1/nodes/{NODE_ID}/delete_confirm
```

The chunk endpoint receives raw `application/octet-stream` bytes. The ESP asks the server to use the AudioMoth bridge chunk size (`MOTH_CHUNK_BYTES`, currently 512 bytes), so each UART `GET` payload maps directly to one server chunk. The ESP signs only the URL path because MothServer authenticates `request.url.path`.

## Command types supported

```text
PING
UPLOAD_NOW
SYNC_MOTH_TIME
MOTH_STATUS
```

`UPLOAD_NOW` bypasses the solar/charging requirement, but still refuses upload below `MIN_UPLOAD_BATTERY_V`.
