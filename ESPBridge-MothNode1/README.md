# Moth_Node_ESPBridge

ESP32-WROOM-U Arduino firmware for the custom AudioMoth Dev ESP bridge firmware.

## What this sketch does

- Connects to personal, WPA2-Enterprise username/password, or open Wi-Fi during wake windows.
- Verifies HTTPS servers using a trusted root certificate and NTP time bootstrap.
- Fetches signed server time from `/v1/public/server_time`.
- Sends HMAC-signed heartbeat and time-check messages.
- Polls queued commands.
- Reads battery voltage on GPIO34.
- Reads charge controller CHRG on GPIO39 and DONE on GPIO36.
- Keeps commands at 115200 baud and negotiates one-way 921600-baud file payloads when supported.
- Requests AudioMoth file service using ESP_REQ on GPIO25 -> AudioMoth a7.
- Respects AudioMoth busy state on GPIO26 <- AudioMoth a8.
- Lists WAV files, fetches them in CRC-checked chunks, uploads chunks to server, and deletes from AudioMoth only after full server confirmation.
- Reports separate SD-read, UART, network, and server-processing timing after each completed file.
- Reads AudioMoth SD total/free space during `LIST` and includes it in the manifest and heartbeat.
- Waits for an AudioMoth `STATUS` response with `allowed=1` before sending `LIST`, `GET`, or `DELETE`, so file transfer does not accidentally run inside the early time-sync bridge window.
- Ignores stale `OK BRIDGE_READY` and `OK PONG` beacon lines while waiting for command-specific replies, keeping `GET` chunk framing aligned.
- Sends the ESP32's current estimated epoch on every bridge retry. This keeps AudioMoth's service-window deadline advancing instead of repeatedly resetting it to the original boot-time server timestamp.
- Signs every server request with the ESP32's current estimated epoch, so long uploads do not age out of the server auth window.
- Resumes interrupted uploads from the server's compact `next_missing_offset` instead of re-sending every already-received chunk.

## Wiring

```text
ESP32-WROOM-U                 AudioMoth Dev
GPIO32 RX2  <---------------- b9  UART TX
GPIO33 TX2  ----------------> b10 UART RX
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

The ESP32 UART setup must leave GPIO32/GPIO33 owned by `Serial2` after `Serial2.begin(...)`. Do not call `pinMode()` on either UART pin after `begin()`, or the ESP32 pin matrix can detach RX2 and the bridge will see GPIO edges but decode zero UART bytes.

## Arduino setup

1. Open the `Moth_Node_ESPBridge` folder in Arduino IDE.
2. Select `ESP32 Dev Module`.
3. Install `ArduinoJson`.
4. Upload the same sketch to every ESP32.

Arduino CLI equivalents:

```powershell
arduino-cli compile --fqbn esp32:esp32:esp32 ".\Moth_Node_ESPBridge"
arduino-cli upload -p COM7 --fqbn esp32:esp32:esp32 ".\Moth_Node_ESPBridge"
```

## Setup and field Wi-Fi recovery

The firmware stores node setup in ESP32 NVS flash. Normal boots skip setup when saved credentials exist. If required credentials are missing, the ESP32 starts a local setup Wi-Fi network:

```text
SSID: BatNode-XXXXXX
Password: batnode-setup
Setup page: http://192.168.4.1
```

Choose the field Wi-Fi security mode, enter the public HTTPS server URL, and
submit enrollment. The dashboard shows the physical ESP32 under **Add Nodes**.
Press **Approve** and the node saves its generated credentials automatically.
No shared provisioning token or copied device secret is required.
The ESP32 polls approval itself every three seconds; the phone or laptop may
leave the setup access point after the request is submitted.

WPA2-Enterprise setup accepts outer identity, username, and password. Networks
that require a browser captive portal are not supported for unattended nodes.

If a configured node cannot join its saved Wi-Fi, it automatically opens the
same `BatNode-XXXXXX` access point for ten minutes. Join it with the password
`batnode-setup`; the captive setup page should open automatically, with
`http://192.168.4.1` as the manual fallback. Enter the replacement personal,
enterprise, or open-network settings and press **Save Wi-Fi and reconnect**.
This Wi-Fi-only path preserves the node ID, server URL, key, device secret, and
dashboard history. It does not require another approval or firmware flash.

The server links the ESP32 eFuse hardware ID to its node record. When settings
are erased or firmware is reflashed, approving the same hardware preserves its
node ID and history while rotating the device credential. During the first
migration from older firmware, select the existing node ID once in the approval
screen.

For internet deployments, use the stable HTTPS URL printed by the server's
`StartInternetAccess.cmd` Tailscale helper. Plain HTTP remains available for
bench testing on a trusted local network.

To open setup while the current network still works, send the `OPEN_SETUP`
dashboard command. Erasing NVS is reserved for intentionally removing the node
identity and starting enrollment again.

## Server endpoints needed for WAV upload

Existing endpoints reused from your current server:

```text
GET  /v1/public/server_time
POST /v1/device/heartbeat
POST /v1/device/time_check
GET  /v1/device/{NODE_ID}/commands
POST /v1/device/{NODE_ID}/commands/{id}/ack
```

The ESP32 asserts ESP_REQ before attempting the UART bridge. That order matters because AudioMoth opens the UART bridge only after seeing the request pin. MOTH_BUSY is treated as an advisory pin and is read with the ESP32 internal pulldown enabled: the ESP32 waits briefly for it to fall, logs a warning if it stays high, then still listens for `OK BRIDGE_READY` and probes with `PING` for up to about 65 seconds. This keeps a noisy or stuck BUSY line from blocking a working UART bridge.

`MOTH_ASSERT_REQ_AT_BOOT` is enabled so ESP_REQ goes high as soon as the ESP32 bridge pins are initialised. This gives the AudioMoth startup request-service firmware a chance to open UART before the ESP32 has finished Wi-Fi, server time sync, and command polling. The pin is driven low again before ESP32 deep sleep.

Upload/delete endpoints this ESP firmware expects from the current MothServer:

```text
POST /v1/files/manifest
POST /v1/uploads/init
PUT  /v1/uploads/{upload_id}/chunks/{chunk_index}
POST /v1/uploads/{upload_id}/complete
GET  /v1/nodes/{NODE_ID}/delete_authorization?manifest_id=...
POST /v1/nodes/{NODE_ID}/delete_confirm
```

The chunk endpoint receives raw `application/octet-stream` bytes. AudioMoth
provides CRC-checked 8192-byte UART payloads. The ESP combines up to eight of
them in a heap-allocated 65536-byte buffer and sends one signed server PUT.
This keeps UART reads small enough for AudioMoth RAM while cutting HTTPS request
overhead by 16 times compared with the old 4096-byte one-request-per-read path.
The ESP signs only the URL path because MothServer authenticates
`request.url.path`.

If an upload is interrupted, `/v1/uploads/init` returns compact resume fields: `total_chunks`, `next_missing_chunk`, `next_missing_offset`, and `received_chunk_count`. The ESP32 starts the next `GET` at `next_missing_offset`; the server still treats duplicate chunks as idempotent retries, but normal restarts avoid re-sending old data.

When the matching AudioMoth firmware sees a `LIST` command, it emits an `SD total_kb=... free_kb=...` line before file entries. The ESP32 logs that size, posts `sd_total_kb`, `sd_free_kb`, and `sd_free_mb` with the manifest, and reports `sd_free_mb` on the next heartbeat.

The matching AudioMoth firmware uses the EFM32 `UART1` hardware route on
PB9/PB10, borrowed from the stock GPS interface resources. GPS support is
disabled so the bridge owns PA7, PA8, PB9, PB10, and UART1. Control starts at
115200 baud for reliable discovery and all commands. The ESP uses `FASTCAP` to
arm one-way 921600-baud transfers for `GETFAST` file payloads. Each 8 KiB payload
uses a 1024-byte training preamble; lower negotiated rates use 128 bytes. Both boards automatically return to
115200. Older AudioMoth bridge firmware falls back to 4 KiB reads at 115200.
The ESP aggregates those UART reads into 64 KiB HTTPS PUT requests to reduce
server round trips while retaining enough contiguous heap for TLS. Live tests
showed that 128 KiB could not allocate and 96 KiB starved the HTTPS client.
The ESP USB debug console continues to use 115200 baud.

## Command types supported

```text
PING
UPLOAD_NOW
SYNC_MOTH_TIME
MOTH_STATUS
OPEN_SETUP
```

`UPLOAD_NOW` bypasses the solar/charging requirement, but still refuses upload below `MIN_UPLOAD_BATTERY_V`.
`OPEN_SETUP` preserves the current node identity and restarts once into the local setup portal so Wi-Fi or server URL can be changed.

## One-command bridge test

After flashing the request-service AudioMoth bin and putting AudioMoth back in CUSTOM/run mode, run:

```bat
RunBridgeStatusTest.cmd
```

Or run the PowerShell script directly with a process-local execution-policy bypass:

```powershell
powershell.exe -NoProfile -ExecutionPolicy Bypass -File .\RunBridgeStatusTest.ps1 -Port COM7 -MonitorSeconds 180
```

The script queues a `MOTH_STATUS` command in the local MothServer SQLite database, resets the ESP32 on COM7, monitors serial at 115200, and writes a log under `logs/`.

Expected pass signal:

```text
Bridge READY after ...
```

If it prints `rx_bytes=0`, `busy_low_seen=0`, and `esp_req=1`, the ESP32 is asserting the request pin and sending UART pings but AudioMoth is not replying. In that case, check that AudioMoth is flashed with the request-service bin and is actually running in CUSTOM mode.

## Upload throughput benchmark

After placing at least one not-yet-uploaded recording on the AudioMoth SD card,
run:

```bat
RunFastUploadBenchmark.cmd
```

The benchmark queues `UPLOAD_NOW`, resets COM7, and records the negotiated UART
rate plus separate UART, server, and end-to-end throughput. Logs are written to
`logs/fast-upload-*.log`.
