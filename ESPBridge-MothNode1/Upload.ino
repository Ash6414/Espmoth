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
  mothRequest(false);
}

bool syncMothTimeOnly(long serverEpoch) {
  PowerState p = readPowerState();
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

  PowerState p = readPowerState();
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
  size_t fileCount = 0;
  bool listed = bridgeList(files, MOTH_MAX_FILES_PER_SESSION, fileCount);
  if (!listed) {
    closeBridgeSession();
    summary.code = UPLOAD_BRIDGE_FAILED;
    summary.message = "LIST failed";
    return summary;
  }

  summary.filesSeen = (uint16_t)fileCount;
  if (fileCount == 0) {
    closeBridgeSession();
    summary.code = UPLOAD_NO_FILES;
    summary.message = "no WAV files listed";
    return summary;
  }

  String manifestId;
  if (!serverPostManifest(serverEpoch, files, fileCount, manifestId)) {
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

  uint32_t offset = 0;
  while (offset < file.size) {
    uint32_t remaining = file.size - offset;
    uint32_t requestBytes = remaining > session.chunkSize ? session.chunkSize : remaining;

    ChunkResult chunk;
    bool gotChunk = bridgeGetChunk(file.path, offset, requestBytes, chunk);
    if (!gotChunk || !chunk.ok || chunk.length == 0 || chunk.length != requestBytes) {
      Serial.printf("GET failed at offset %lu\n", (unsigned long)offset);
      bridgeFailure = true;
      return false;
    }

    if (!serverUploadChunk(serverEpoch, session, chunk)) {
      Serial.printf("serverUploadChunk failed at offset %lu\n", (unsigned long)offset);
      return false;
    }

    offset += chunk.length;
    if ((offset % (16UL * 1024UL)) == 0 || offset >= file.size) {
      Serial.printf("Progress %s: %lu/%lu\n", file.path.c_str(), (unsigned long)offset, (unsigned long)file.size);
    }

    PowerState p = readPowerState();
    if (p.batteryV < MIN_WIFI_BATTERY_V) {
      Serial.println("Battery dropped below Wi-Fi threshold during upload");
      return false;
    }
  }

  if (!serverFinishFile(serverEpoch, session)) {
    Serial.println("serverFinishFile failed");
    return false;
  }

  return true;
}
