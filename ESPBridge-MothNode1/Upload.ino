bool waitForMothIdle(uint32_t waitMs) {
  uint32_t start = millis();
  while (millis() - start < waitMs) {
    if (!mothBusy()) return true;
    delay(50);
  }
  return !mothBusy();
}

bool openBridgeSession(long serverEpoch) {
  if (!waitForMothIdle(MOTH_BUSY_WAIT_MS)) return false;

  bridgeFlushInput();
  mothRequest(true);

  if (!bridgeWaitReady(MOTH_READY_TIMEOUT_MS)) {
    mothRequest(false);
    return false;
  }

  if (!bridgePing()) {
    bridgeDone();
    mothRequest(false);
    return false;
  }

  uint32_t epoch = (uint32_t)(serverEpoch > 1700000000L ? serverEpoch : estimatedEpochUtc());
  if (epoch > 1700000000UL) {
    bridgeSetTime(epoch, 0);
  }

  return true;
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
  uint32_t epoch = (uint32_t)(serverEpoch > 1700000000L ? serverEpoch : estimatedEpochUtc());
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

  if (mothBusy()) {
    summary.code = UPLOAD_SKIPPED_BUSY;
    summary.message = "AudioMoth busy; upload skipped";
    return summary;
  }

  if (!openBridgeSession(serverEpoch)) {
    summary.code = UPLOAD_BRIDGE_FAILED;
    summary.message = "bridge session failed";
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

  bool anyServerFailure = false;
  bool anyBridgeFailure = false;

  for (size_t i = 0; i < fileCount; i++) {
    Serial.printf("Uploading %s (%lu bytes)\n", files[i].path.c_str(), (unsigned long)files[i].size);

    bool ok = uploadOneFile(serverEpoch, files[i]);
    if (ok) {
      summary.filesUploaded += 1;
#if DELETE_AFTER_CONFIRMED_UPLOAD
      if (bridgeDelete(files[i].path)) {
        summary.filesDeleted += 1;
      } else {
        anyBridgeFailure = true;
      }
#endif
    } else {
      anyServerFailure = true;
      // Continue to next file. A failed file is never deleted.
    }
  }

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

bool uploadOneFile(long serverEpoch, const MothFile &file) {
  if (file.size == 0) return false;

  if (!serverBeginFile(serverEpoch, file)) {
    Serial.println("serverBeginFile failed");
    return false;
  }

  uint32_t offset = 0;
  while (offset < file.size) {
    uint32_t remaining = file.size - offset;
    uint32_t requestBytes = remaining > MOTH_CHUNK_BYTES ? MOTH_CHUNK_BYTES : remaining;

    ChunkResult chunk;
    bool gotChunk = bridgeGetChunk(file.path, offset, requestBytes, chunk);
    if (!gotChunk || !chunk.ok || chunk.length == 0) {
      Serial.printf("GET failed at offset %lu\n", (unsigned long)offset);
      return false;
    }

    if (!serverUploadChunk(serverEpoch, file, chunk)) {
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

  if (!serverFinishFile(serverEpoch, file)) {
    Serial.println("serverFinishFile failed");
    return false;
  }

  return true;
}
