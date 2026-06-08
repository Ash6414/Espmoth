bool postHeartbeat(long serverEpoch, const PowerState &p, const UploadSummary &upload) {
  StaticJsonDocument<1280> doc;
  doc["node_id"] = NODE_ID;
  doc["battery_v"] = p.batteryV;
  doc["battery_percent"] = p.batteryPercent;
  doc["solar_v"] = nullptr;
  doc["charging"] = p.charging;
  doc["charge_done"] = p.chargeDone;
  doc["recently_charged"] = p.charging || p.chargeDone;
  doc["sd_free_mb"] = nullptr;
  doc["recording_status"] = mothBusy() ? "MOTH_BUSY" : "MOTH_IDLE";
  doc["upload_status"] = upload.message;
  doc["wifi_rssi_dbm"] = WiFi.RSSI();
  doc["mode"] = "ESPBRIDGE_UART_UPLOAD";

  JsonObject bridge = doc.createNestedObject("bridge");
  bridge["req_pin"] = digitalRead(PIN_MOTH_REQ);
  bridge["busy_pin"] = digitalRead(PIN_MOTH_BUSY);
  bridge["uart_baud"] = MOTH_UART_BAUD;

  JsonObject stats = doc.createNestedObject("stats");
  stats["boot_count"] = rtcBootCounter;
  stats["successful_upload_sessions"] = rtcSuccessfulUploads;
  stats["failed_upload_sessions"] = rtcFailedUploads;
  stats["files_seen_last"] = upload.filesSeen;
  stats["files_uploaded_last"] = upload.filesUploaded;
  stats["files_deleted_last"] = upload.filesDeleted;
  stats["estimated_epoch"] = estimatedEpochUtc();

  String body;
  serializeJson(doc, body);
  String response;
  return signedPostJson(ENDPOINT_HEARTBEAT, body, serverEpoch, response);
}

bool postTimeCheck(long serverEpoch, uint32_t rttMs, const String &notes) {
  StaticJsonDocument<512> doc;
  doc["node_id"] = NODE_ID;
  doc["server_epoch"] = serverEpoch;
  doc["esp_epoch_after"] = estimatedEpochUtc();
  doc["audiomoth_epoch"] = nullptr;
  doc["rtt_ms"] = rttMs;
  doc["time_source"] = "server_to_esp32_to_audiomoth_uart_time_command";
  doc["notes"] = notes;

  String body;
  serializeJson(doc, body);
  String response;
  return signedPostJson(ENDPOINT_TIME_CHECK, body, serverEpoch, response);
}

void ackCommand(long serverEpoch, int commandId, const String &msg) {
  if (commandId < 0) return;

  StaticJsonDocument<384> doc;
  JsonObject response = doc.createNestedObject("response");
  response["message"] = msg;
  response["estimated_epoch"] = estimatedEpochUtc();
  response["moth_busy"] = mothBusy();

  String body;
  serializeJson(doc, body);
  String path = String("/v1/device/") + NODE_ID + "/commands/" + String(commandId) + "/ack";
  String resp;
  signedPostJson(path, body, serverEpoch, resp);
}

void pollCommands(long serverEpoch, const PowerState &p) {
  String path = String("/v1/device/") + NODE_ID + "/commands";
  String resp;
  if (!signedGet(path, serverEpoch, resp)) return;

  StaticJsonDocument<1536> doc;
  DeserializationError err = deserializeJson(doc, resp);
  if (err) {
    Serial.println("Command JSON parse failed");
    return;
  }

  JsonArray cmds = doc["commands"].as<JsonArray>();
  for (JsonObject cmd : cmds) {
    int id = cmd["id"] | -1;
    String type = cmd["type"] | "";

    if (type == "PING") {
      postHeartbeat(serverEpoch, p, lastUpload);
      ackCommand(serverEpoch, id, "PING handled; fresh heartbeat posted");
    } else if (type == "UPLOAD_NOW") {
      if (!powerAllowsUpload(p, true)) {
        ackCommand(serverEpoch, id, "UPLOAD_NOW refused by battery threshold");
      } else {
        UploadSummary forced = runAudioMothUploadSession(serverEpoch, true);
        lastUpload = forced;
        postHeartbeat(serverEpoch, p, forced);
        ackCommand(serverEpoch, id, forced.message);
      }
    } else if (type == "SYNC_MOTH_TIME") {
      bool ok = syncMothTimeOnly(serverEpoch);
      ackCommand(serverEpoch, id, ok ? "AudioMoth TIME command sent" : "AudioMoth TIME command failed or bridge not ready");
    } else if (type == "MOTH_STATUS") {
      String status;
      bool ok = false;
      if (!mothBusy()) {
        mothRequest(true);
        ok = bridgeWaitReady(MOTH_READY_TIMEOUT_MS) && bridgeStatus(status);
        bridgeDone();
        mothRequest(false);
      }
      ackCommand(serverEpoch, id, ok ? status : String("AudioMoth status unavailable"));
    } else {
      ackCommand(serverEpoch, id, "Unknown command ignored");
    }
  }
}

bool serverBeginFile(long serverEpoch, const MothFile &file) {
  StaticJsonDocument<512> doc;
  doc["node_id"] = NODE_ID;
  doc["path"] = file.path;
  doc["size"] = file.size;
  doc["chunk_bytes"] = MOTH_CHUNK_BYTES;
  doc["started_epoch"] = estimatedEpochUtc();

  String body;
  serializeJson(doc, body);
  String response;
  return signedPostJson(ENDPOINT_UPLOAD_START, body, serverEpoch, response);
}

bool serverUploadChunk(long serverEpoch, const MothFile &file, const ChunkResult &chunk) {
  String path = String(ENDPOINT_UPLOAD_CHUNK) +
                "?node_id=" + urlEncode(NODE_ID) +
                "&path=" + urlEncode(file.path) +
                "&offset=" + String(chunk.offset) +
                "&length=" + String(chunk.length) +
                "&total=" + String(file.size) +
                "&crc32=" + String(chunk.crc, HEX);

  String response;
  return signedPostBinary(path, mothChunk, chunk.length, serverEpoch, response);
}

bool serverFinishFile(long serverEpoch, const MothFile &file) {
  StaticJsonDocument<512> doc;
  doc["node_id"] = NODE_ID;
  doc["path"] = file.path;
  doc["size"] = file.size;
  doc["finished_epoch"] = estimatedEpochUtc();

  String body;
  serializeJson(doc, body);
  String response;
  return signedPostJson(ENDPOINT_UPLOAD_FINISH, body, serverEpoch, response);
}
