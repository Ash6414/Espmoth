bool waitForMothIdle(uint32_t waitMs) {
  uint32_t start = millis();
  while (millis() - start < waitMs) {
    if (!mothBusy()) return true;
    delay(50);
  }
  return !mothBusy();
}

uint32_t bridgeEpochNow(long serverEpoch) {
  uint32_t estimated = estimatedEpochUtc();
  if (estimated > 1700000000UL) return estimated;
  if (serverEpoch > 1700000000L) return (uint32_t)serverEpoch;
  return 0;
}

bool openBridgeSession(long serverEpoch) {
  bridgeFlushInput();
  mothRequest(true);

  if (!waitForMothIdle(MOTH_BUSY_WAIT_MS)) {
    Serial.printf("AudioMoth kept MOTH_BUSY high for %lu ms after ESP_REQ; trying UART READY anyway (MOTH_BUSY=%d ESP_REQ=%d)\n",
                  (unsigned long)MOTH_BUSY_WAIT_MS, mothBusy(), digitalRead(PIN_MOTH_REQ));
  }

  if (!bridgeWaitReady(MOTH_READY_TIMEOUT_MS)) {
    Serial.printf("AudioMoth did not send OK BRIDGE_READY; MOTH_BUSY=%d ESP_REQ=%d. Check AudioMoth switch is CUSTOM and bridge service window is active.\n",
                  mothBusy(), digitalRead(PIN_MOTH_REQ));
    mothRequest(false);
    return false;
  }

  if (!bridgePing()) {
    Serial.println("AudioMoth bridge PING failed");
    bridgeDone();
    mothRequest(false);
    return false;
  }

  if (!bridgeEnableFastBaud()) {
    mothRequest(false);
    return false;
  }

  uint32_t epoch = bridgeEpochNow(serverEpoch);
  if (epoch > 1700000000UL) {
    bridgeSetTime(epoch, 0);
  }

  return true;
}

bool bridgeStatusAllowsUpload(const String &status) {
  return status.indexOf("allowed=1") >= 0;
}

bool openBridgeUploadSession(long serverEpoch) {
  uint32_t start = millis();
  uint32_t attempt = 0;

  while (millis() - start < MOTH_UPLOAD_WINDOW_WAIT_MS) {
    attempt += 1;
    if (!openBridgeSession(serverEpoch)) {
      delay(MOTH_UPLOAD_WINDOW_RETRY_MS);
      continue;
    }

    String status;
    bool statusOk = bridgeStatus(status);
    if (statusOk && bridgeStatusAllowsUpload(status)) {
      Serial.printf("AudioMoth file service ready after %lu attempt(s): %s\n",
                    (unsigned long)attempt, status.c_str());
      return true;
    }

    Serial.printf("AudioMoth bridge is alive but file service is not ready on attempt %lu: %s\n",
                  (unsigned long)attempt, statusOk ? status.c_str() : "STATUS unavailable");
    closeBridgeSession();
    delay(MOTH_UPLOAD_WINDOW_RETRY_MS);
  }

  Serial.println("AudioMoth file service did not become upload-ready before timeout");
  return false;
}

void closeBridgeSession() {
  bridgeDone();
  delay(50);
  bridgeRestoreDefaultBaud();
  mothRequest(false);
}

bool syncMothTimeOnly(long serverEpoch) {
  PowerState p = preWifiPowerValid ? preWifiPowerState : readPowerState();
  if (p.batteryV < MIN_MOTH_TIME_SYNC_V) return false;

  if (!openBridgeSession(serverEpoch)) return false;
  uint32_t epoch = bridgeEpochNow(serverEpoch);
  bool ok = bridgeSetTime(epoch, 0);
  closeBridgeSession();
  return ok;
}

UploadSummary runAudioMothUploadSession(long serverEpoch, bool forced) {
  UploadSummary summary;
  summary.code = UPLOAD_NOT_ATTEMPTED;
  summary.filesSeen = 0;
  summary.filesUploaded = 0;
  summary.filesDeleted = 0;
  summary.message = "upload not attempted";
  summary.sd = {false, 0, 0};

  PowerState p = preWifiPowerValid ? preWifiPowerState : readPowerState();
  if (!powerAllowsUpload(p, forced)) {
    summary.code = UPLOAD_SKIPPED_POWER;
    summary.message = "upload skipped by power policy";
    return summary;
  }

  if (!openBridgeUploadSession(serverEpoch)) {
    summary.code = UPLOAD_BRIDGE_FAILED;
    summary.message = "AudioMoth file service not ready";
    return summary;
  }

  MothFile files[MOTH_MAX_FILES_PER_SESSION];
  MothSdInfo sdInfo = {false, 0, 0};
  size_t fileCount = 0;
  bool listed = bridgeList(files, MOTH_MAX_FILES_PER_SESSION, fileCount, &sdInfo);
  if (!listed) {
    closeBridgeSession();
    summary.code = UPLOAD_BRIDGE_FAILED;
    summary.message = "LIST failed";
    return summary;
  }

  summary.sd = sdInfo;
  summary.filesSeen = (uint16_t)fileCount;
  if (fileCount == 0) {
    closeBridgeSession();
    summary.code = UPLOAD_NO_FILES;
    summary.message = "no files listed";
    return summary;
  }

  String manifestId;
  if (!serverPostManifest(serverEpoch, files, fileCount, sdInfo, manifestId)) {
    closeBridgeSession();
    summary.code = UPLOAD_SERVER_FAILED;
    summary.message = "manifest post failed";
    return summary;
  }

  bool anyServerFailure = false;
  bool anyBridgeFailure = false;

  for (size_t i = 0; i < fileCount; i++) {
    Serial.printf("Uploading %s (%lu bytes)\n", files[i].path.c_str(), (unsigned long)files[i].size);

    bool bridgeFailure = false;
    bool ok = uploadOneFile(serverEpoch, manifestId, files[i], bridgeFailure);
    if (ok) {
      summary.filesUploaded += 1;
    } else {
      if (bridgeFailure) anyBridgeFailure = true;
      else anyServerFailure = true;
      // Continue to next file. A failed file is never deleted.
    }
  }

#if DELETE_AFTER_CONFIRMED_UPLOAD
  if (summary.filesUploaded > 0) {
    DeleteCandidate deletes[MOTH_MAX_FILES_PER_SESSION];
    size_t deleteCount = 0;
    String authorizationId;

    if (serverFetchDeleteAuthorization(serverEpoch, manifestId, files, fileCount, deletes, MOTH_MAX_FILES_PER_SESSION, deleteCount, authorizationId)) {
      for (size_t i = 0; i < deleteCount; i++) {
        if (bridgeDelete(deletes[i].path)) {
          deletes[i].deleted = true;
          summary.filesDeleted += 1;
        } else {
          deletes[i].deleted = false;
          deletes[i].error = "AudioMoth DELETE failed";
          anyBridgeFailure = true;
        }
      }

      if (deleteCount > 0 && !serverConfirmDeletes(serverEpoch, authorizationId, deletes, deleteCount)) {
        anyServerFailure = true;
      }
    } else {
      anyServerFailure = true;
    }
  }
#endif

  closeBridgeSession();

  if (summary.filesUploaded > 0 && !anyServerFailure && !anyBridgeFailure) {
    summary.code = UPLOAD_SUCCESS;
    summary.message = "upload session complete";
  } else if (summary.filesUploaded > 0) {
    summary.code = anyServerFailure ? UPLOAD_SERVER_FAILED : UPLOAD_BRIDGE_FAILED;
    summary.message = "partial upload session; some files failed";
  } else {
    summary.code = anyServerFailure ? UPLOAD_SERVER_FAILED : UPLOAD_BRIDGE_FAILED;
    summary.message = anyServerFailure ? "server upload failed" : "bridge upload failed";
  }

  return summary;
}

bool uploadOneFile(long serverEpoch, const String &manifestId, const MothFile &file, bool &bridgeFailure) {
  bridgeFailure = false;
  if (file.size == 0) return false;

  UploadSession session;
  if (!serverInitFile(serverEpoch, manifestId, file, session)) {
    Serial.println("serverInitFile failed");
    return false;
  }

  if (session.alreadyComplete) {
    Serial.printf("Server already has %s\n", file.path.c_str());
    return true;
  }

  if (!serverChunk) {
    serverChunk = (uint8_t *)malloc(SERVER_UPLOAD_CHUNK_BYTES);
    if (!serverChunk) {
      Serial.printf("Could not allocate %u-byte upload buffer\n", SERVER_UPLOAD_CHUNK_BYTES);
      return false;
    }
  }

  uint32_t offset = session.resumeOffset;
  uint32_t transferStartMs = millis();
  while (offset < file.size) {
    uint32_t remaining = file.size - offset;
    uint32_t batchBytes = remaining > session.chunkSize ? session.chunkSize : remaining;
    uint32_t filled = 0;

    while (filled < batchBytes) {
      uint32_t uartRemaining = batchBytes - filled;
      uint32_t requestBytes = uartRemaining > MOTH_CHUNK_BYTES ? MOTH_CHUNK_BYTES : uartRemaining;
      uint32_t uartOffset = offset + filled;

      ChunkResult chunk;
      bool gotChunk = bridgeGetChunk(file.path, uartOffset, requestBytes, chunk);
      if (!gotChunk || !chunk.ok || chunk.offset != uartOffset || chunk.length == 0 || chunk.length != requestBytes) {
        Serial.printf("GET failed at offset %lu\n", (unsigned long)uartOffset);
        bridgeFailure = true;
        return false;
      }

      memcpy(serverChunk + filled, mothChunk, chunk.length);
      filled += chunk.length;
    }

    if (!serverUploadChunk(serverEpoch, session, serverChunk, offset, batchBytes)) {
      Serial.printf("serverUploadChunk failed at offset %lu\n", (unsigned long)offset);
      return false;
    }

    offset += batchBytes;
    if ((offset % (256UL * 1024UL)) == 0 || offset >= file.size) {
      Serial.printf("Progress %s: %lu/%lu\n", file.path.c_str(), (unsigned long)offset, (unsigned long)file.size);
    }

    PowerState p = readPowerState();
    if (p.batteryV < MIN_ACTIVE_BATTERY_V) {
      Serial.printf("Battery under load fell below emergency cutoff: %.3f V\n", p.batteryV);
      return false;
    }
  }

  if (!serverFinishFile(serverEpoch, session)) {
    Serial.println("serverFinishFile failed");
    return false;
  }

  uint32_t elapsedMs = millis() - transferStartMs;
  float kibPerSecond = elapsedMs > 0 ? ((float)(file.size - session.resumeOffset) * 1000.0f) / (1024.0f * (float)elapsedMs) : 0.0f;
  Serial.printf("Completed %s in %.1f s (%.1f KiB/s)\n",
                file.path.c_str(), elapsedMs / 1000.0f, kibPerSecond);

  return true;
}
