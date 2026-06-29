bool postHeartbeat(long serverEpoch, const PowerState &p, const UploadSummary &upload) {
  StaticJsonDocument<1280> doc;
  doc["node_id"] = cfgNodeId();
  doc["battery_v"] = p.batteryV;
  doc["battery_percent"] = p.batteryPercent;
  doc["charging"] = p.charging;
  doc["charge_done"] = p.chargeDone;
  doc["recently_charged"] = p.charging || p.chargeDone;
  if (upload.sd.valid) doc["sd_free_mb"] = (double)upload.sd.freeKb / 1024.0;
  else doc["sd_free_mb"] = nullptr;
  doc["recording_status"] = mothBusy() ? "MOTH_BUSY" : "MOTH_IDLE";
  doc["upload_status"] = upload.message;
  doc["wifi_rssi_dbm"] = WiFi.RSSI();
  doc["mode"] = "ESPBRIDGE_UART_UPLOAD";

  JsonObject bridge = doc.createNestedObject("bridge");
  bridge["req_pin"] = digitalRead(PIN_MOTH_REQ);
  bridge["busy_pin"] = digitalRead(PIN_MOTH_BUSY);
  bridge["uart_baud"] = bridgeCurrentBaudRate();

  JsonObject stats = doc.createNestedObject("stats");
  stats["boot_count"] = rtcBootCounter;
  stats["successful_upload_sessions"] = rtcSuccessfulUploads;
  stats["failed_upload_sessions"] = rtcFailedUploads;
  stats["files_seen_last"] = upload.filesSeen;
  stats["files_uploaded_last"] = upload.filesUploaded;
  stats["files_deleted_last"] = upload.filesDeleted;
  stats["estimated_epoch"] = estimatedEpochUtc();
  stats["battery_measurement"] = "pre_wifi_trimmed_average";
  if (upload.sd.valid) {
    stats["sd_total_mb"] = upload.sd.totalKb / 1024UL;
    stats["sd_free_mb"] = upload.sd.freeKb / 1024UL;
  }

  String body;
  serializeJson(doc, body);
  String response;
  return signedPostJson(ENDPOINT_HEARTBEAT, body, serverEpoch, response);
}

bool postTimeCheck(long serverEpoch, uint32_t rttMs, const String &notes) {
  StaticJsonDocument<512> doc;
  doc["node_id"] = cfgNodeId();
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
  String path = String("/v1/device/") + cfgNodeId() + "/commands/" + String(commandId) + "/ack";
  String resp;
  signedPostJson(path, body, serverEpoch, resp);
}

void pollCommands(long serverEpoch, const PowerState &p) {
  String path = String("/v1/device/") + cfgNodeId() + "/commands";
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
      if (openBridgeSession(serverEpoch)) {
        ok = bridgeStatus(status);
        closeBridgeSession();
      }
      ackCommand(serverEpoch, id, ok ? status : String("AudioMoth status unavailable"));
    } else if (type == "OPEN_SETUP") {
      ackCommand(serverEpoch, id, "Restarting into Bat Node setup portal");
      requestProvisioningOnNextBoot();
      delay(600);
      ESP.restart();
    } else {
      ackCommand(serverEpoch, id, "Unknown command ignored");
    }
  }
}

String serverManifestId() {
  return cfgNodeId() + "-AUDIOMOTH-SD";
}

String serverFilenameFromPath(const String &path) {
  int slash = path.lastIndexOf('/');
  int backslash = path.lastIndexOf('\\');
  int start = max(slash, backslash) + 1;
  return path.substring(start);
}

uint32_t serverLocalFileId(const MothFile &file) {
  uint32_t crc = crc32Update(0, (const uint8_t *)file.path.c_str(), file.path.length());
  uint8_t sizeBytes[4] = {
    (uint8_t)(file.size & 0xFF),
    (uint8_t)((file.size >> 8) & 0xFF),
    (uint8_t)((file.size >> 16) & 0xFF),
    (uint8_t)((file.size >> 24) & 0xFF)
  };
  crc = crc32Update(crc, sizeBytes, sizeof(sizeBytes));
  return crc == 0 ? 1 : crc;
}

bool serverPostManifest(long serverEpoch, MothFile *files, size_t fileCount, const MothSdInfo &sdInfo, String &manifestIdOut) {
  manifestIdOut = serverManifestId();

  DynamicJsonDocument doc(1280 + fileCount * 256);
  doc["node_id"] = cfgNodeId();
  doc["manifest_id"] = manifestIdOut;
  doc["deployment_id"] = nullptr;
  doc["sd_card_id"] = "AudioMoth";
  if (sdInfo.valid) {
    doc["sd_total_kb"] = sdInfo.totalKb;
    doc["sd_free_kb"] = sdInfo.freeKb;
    doc["sd_total_mb"] = sdInfo.totalKb / 1024UL;
    doc["sd_free_mb"] = (double)sdInfo.freeKb / 1024.0;
  }

  JsonArray arr = doc.createNestedArray("files");
  for (size_t i = 0; i < fileCount; i++) {
    files[i].localFileId = serverLocalFileId(files[i]);

    JsonObject item = arr.createNestedObject();
    item["local_file_id"] = files[i].localFileId;
    item["filename"] = serverFilenameFromPath(files[i].path);
    item["file_size_bytes"] = files[i].size;
    item["recorded_at"] = nullptr;
  }

  String body;
  serializeJson(doc, body);

  String response;
  if (!signedPostJson(ENDPOINT_FILES_MANIFEST, body, serverEpoch, response)) return false;

  StaticJsonDocument<512> resp;
  DeserializationError err = deserializeJson(resp, response);
  return !err && (resp["ok"] | false);
}

bool serverInitFile(long serverEpoch, const String &manifestId, const MothFile &file, UploadSession &session) {
  session.ok = false;
  session.alreadyComplete = false;
  session.uploadId = "";
  session.fileId = 0;
  session.chunkSize = 0;
  session.totalChunks = 0;
  session.resumeOffset = 0;

  StaticJsonDocument<512> doc;
  doc["manifest_id"] = manifestId;
  doc["local_file_id"] = file.localFileId;
  doc["filename"] = serverFilenameFromPath(file.path);
  doc["file_size_bytes"] = file.size;
  doc["chunk_size"] = SERVER_UPLOAD_CHUNK_BYTES;

  String body;
  serializeJson(doc, body);

  String response;
  if (!signedPostJson(ENDPOINT_UPLOAD_INIT, body, serverEpoch, response)) return false;

  StaticJsonDocument<128> filter;
  filter["ok"] = true;
  filter["already_complete"] = true;
  filter["file_id"] = true;
  filter["upload_id"] = true;
  filter["chunk_size"] = true;
  filter["total_chunks"] = true;
  filter["next_missing_offset"] = true;

  StaticJsonDocument<768> resp;
  DeserializationError err = deserializeJson(resp, response, DeserializationOption::Filter(filter));
  if (err || !(resp["ok"] | false)) return false;

  session.alreadyComplete = resp["already_complete"] | false;
  session.fileId = resp["file_id"] | 0;
  session.ok = true;

  if (session.alreadyComplete) return true;

  const char *uploadId = resp["upload_id"] | "";
  session.uploadId = String(uploadId);
  session.chunkSize = resp["chunk_size"] | 0;
  session.totalChunks = resp["total_chunks"] | 0;
  session.resumeOffset = resp["next_missing_offset"] | 0;

  if (session.uploadId.length() == 0 || session.chunkSize == 0) return false;
  if (session.chunkSize != SERVER_UPLOAD_CHUNK_BYTES) {
    Serial.printf("Server chunk size %lu does not match requested upload chunk size %u\n",
                  (unsigned long)session.chunkSize, SERVER_UPLOAD_CHUNK_BYTES);
    return false;
  }
  if (session.resumeOffset > file.size) {
    Serial.printf("Server resume offset %lu is beyond file size %lu\n",
                  (unsigned long)session.resumeOffset, (unsigned long)file.size);
    return false;
  }
  if (session.resumeOffset < file.size && (session.resumeOffset % session.chunkSize) != 0) {
    Serial.printf("Server resume offset %lu is not aligned to chunk size %lu\n",
                  (unsigned long)session.resumeOffset, (unsigned long)session.chunkSize);
    return false;
  }
  if (session.resumeOffset > 0) {
    Serial.printf("Resuming %s at byte %lu\n", file.path.c_str(), (unsigned long)session.resumeOffset);
  }

  return true;
}

bool serverUploadChunk(long serverEpoch, const UploadSession &session, const uint8_t *data, uint32_t offset, uint32_t length, uint32_t &serverProcessMs) {
  serverProcessMs = 0;
  if (!session.ok || session.uploadId.length() == 0 || session.chunkSize == 0) return false;
  if (!data || length == 0 || length > session.chunkSize) return false;
  if (offset % session.chunkSize != 0) return false;

  uint32_t chunkIndex = offset / session.chunkSize;
  String path = String("/v1/uploads/") + session.uploadId + "/chunks/" + String(chunkIndex);

  String response;
  if (!signedPutBinary(path, data, length, serverEpoch, response)) return false;

  StaticJsonDocument<128> doc;
  if (!deserializeJson(doc, response)) {
    serverProcessMs = doc["server_ms"] | 0;
  }
  return true;
}

bool serverFinishFile(long serverEpoch, const UploadSession &session) {
  if (!session.ok) return false;
  if (session.alreadyComplete) return true;
  if (session.uploadId.length() == 0) return false;

  String body = "{}";
  String path = String("/v1/uploads/") + session.uploadId + "/complete";
  String response;
  if (!signedPostJson(path, body, serverEpoch, response)) return false;

  StaticJsonDocument<1024> resp;
  DeserializationError err = deserializeJson(resp, response);
  return !err && (resp["ok"] | false);
}

bool serverFetchDeleteAuthorization(long serverEpoch, const String &manifestId, MothFile *files, size_t fileCount, DeleteCandidate *candidates, size_t maxCandidates, size_t &candidateCount, String &authorizationId) {
  candidateCount = 0;
  authorizationId = "";

  String path = String("/v1/nodes/") + cfgNodeId() + "/delete_authorization?manifest_id=" + urlEncode(manifestId);
  String response;
  if (!signedGet(path, serverEpoch, response)) return false;

  DynamicJsonDocument doc(4096);
  DeserializationError err = deserializeJson(doc, response);
  if (err || !(doc["ok"] | false)) return false;

  const char *auth = doc["authorization_id"] | "";
  authorizationId = String(auth);
  if (authorizationId.length() == 0) return false;

  JsonArray safeFiles = doc["files"].as<JsonArray>();
  for (JsonObject item : safeFiles) {
    if (candidateCount >= maxCandidates) break;

    uint32_t localFileId = item["local_file_id"] | 0;
    uint32_t fileId = item["file_id"] | 0;
    if (localFileId == 0 || fileId == 0) continue;

    for (size_t i = 0; i < fileCount; i++) {
      if (files[i].localFileId != localFileId) continue;

      const char *filename = item["filename"] | "";
      candidates[candidateCount].fileId = fileId;
      candidates[candidateCount].localFileId = localFileId;
      candidates[candidateCount].filename = String(filename);
      candidates[candidateCount].path = files[i].path;
      candidates[candidateCount].deleted = false;
      candidates[candidateCount].error = "";
      candidateCount += 1;
      break;
    }
  }

  return true;
}

bool serverConfirmDeletes(long serverEpoch, const String &authorizationId, DeleteCandidate *candidates, size_t candidateCount) {
  if (authorizationId.length() == 0) return false;

  DynamicJsonDocument doc(512 + candidateCount * 256);
  doc["authorization_id"] = authorizationId;

  JsonArray arr = doc.createNestedArray("files");
  for (size_t i = 0; i < candidateCount; i++) {
    JsonObject item = arr.createNestedObject();
    item["file_id"] = candidates[i].fileId;
    item["local_file_id"] = candidates[i].localFileId;
    item["filename"] = candidates[i].filename;
    item["result"] = candidates[i].deleted ? "DELETED" : "FAILED";
    if (candidates[i].error.length()) item["error"] = candidates[i].error;
    else item["error"] = nullptr;
  }

  String body;
  serializeJson(doc, body);

  String path = String("/v1/nodes/") + cfgNodeId() + "/delete_confirm";
  String response;
  return signedPostJson(path, body, serverEpoch, response);
}
