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
    if (statusOk) bridgeApplyStatusCapabilities(status);
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

String runAudioMothListDiagnostic(long serverEpoch) {
  MothFile files[MOTH_MAX_FILES_PER_SESSION];
  MothSdInfo sdInfo = {false, 0, 0};
  size_t fileCount = 0;

  if (!openBridgeUploadSession(serverEpoch)) {
    return "MOTH_LIST failed: AudioMoth file service not ready";
  }

  bool listed = bridgeList(files, MOTH_MAX_FILES_PER_SESSION, fileCount, &sdInfo);
  closeBridgeSession();
  if (!listed) {
    return "MOTH_LIST failed: LIST command failed";
  }

  String msg = "MOTH_LIST count=" + String((unsigned)fileCount);
  if (sdInfo.valid) {
    msg += " sd_free_mb=" + String(sdInfo.freeKb / 1024UL);
    msg += " sd_total_mb=" + String(sdInfo.totalKb / 1024UL);
  }
  size_t previewCount = fileCount < 3 ? fileCount : 3;
  for (size_t i = 0; i < previewCount; i += 1) {
    msg += " file" + String((unsigned)i + 1) + "=" + files[i].path + ":" + String(files[i].size);
  }
  if (fileCount > previewCount) {
    msg += " more=" + String((unsigned)(fileCount - previewCount));
  }
  return msg;
}

String runAudioMothTestStreamDiagnostic(long serverEpoch) {
  const uint32_t bauds[] = {
    MOTH_STREAM_TEST_BAUD_3,
    MOTH_STREAM_TEST_BAUD_2,
    MOTH_STREAM_TEST_BAUD_1,
  };
  uint32_t lastFailBaud = 0;

  for (size_t i = 0; i < sizeof(bauds) / sizeof(bauds[0]); i += 1) {
    uint32_t baud = bauds[i];
    if (!openBridgeSession(serverEpoch)) {
      lastFailBaud = baud;
      continue;
    }

    uint32_t received = 0;
    uint32_t elapsedMs = 0;
    uint32_t crc = 0;

    bool ok = bridgeRunTestStream(MOTH_TEST_STREAM_BYTES, baud, received, elapsedMs, crc);
    closeBridgeSession();
    if (!ok) {
      lastFailBaud = baud;
      Serial.printf("MOTH_TEST_STREAM failed at %lu baud; trying lower baud if available\n", (unsigned long)baud);
      if (i + 1 < sizeof(bauds) / sizeof(bauds[0])) {
        delay(baud >= 921600UL ? 15000 : 30000);
      }
      continue;
    }

    float kibPerSecond = elapsedMs > 0 ? ((float)received * 1000.0f) / (1024.0f * (float)elapsedMs) : 0.0f;
    Serial.printf("MOTH_TEST_STREAM OK bytes=%lu baud=%lu ms=%lu rate=%.1f KiB/s crc=%08lX\n",
                  (unsigned long)received,
                  (unsigned long)baud,
                  (unsigned long)elapsedMs,
                  kibPerSecond,
                  (unsigned long)crc);

    String msg = "MOTH_TEST_STREAM ok bytes=" + String(received);
    msg += " baud=" + String(baud);
    msg += " ms=" + String(elapsedMs);
    msg += " kib_s=" + String(kibPerSecond, 1);
    msg += " crc=" + String(crc, HEX);
    if (lastFailBaud != 0) {
      msg += " failed_higher_baud=" + String(lastFailBaud);
    }
    return msg;
  }

  return "MOTH_TEST_STREAM failed all_bauds";
}

UploadOptions defaultUploadOptions() {
  UploadOptions options;
  options.preferSmallest = false;
  options.maxFiles = 0;
  options.minFileBytes = 0;
  options.maxFileBytes = 0;
  options.targetPath = "";
  return options;
}

bool uploadOptionsActive(const UploadOptions &options) {
  return options.preferSmallest ||
         options.maxFiles > 0 ||
         options.minFileBytes > 0 ||
         options.maxFileBytes > 0 ||
         options.targetPath.length() > 0;
}

String uploadBaseName(const String &path) {
  int slash = path.lastIndexOf('/');
  int backslash = path.lastIndexOf('\\');
  int start = max(slash, backslash) + 1;
  return path.substring(start);
}

bool uploadFileMatchesOptions(const MothFile &file, const UploadOptions &options) {
  if (options.targetPath.length() > 0 &&
      !file.path.equalsIgnoreCase(options.targetPath) &&
      !uploadBaseName(file.path).equalsIgnoreCase(options.targetPath)) {
    return false;
  }
  if (options.minFileBytes > 0 && file.size < options.minFileBytes) return false;
  if (options.maxFileBytes > 0 && file.size > options.maxFileBytes) return false;
  return true;
}

void sortUploadFilesBySize(MothFile *files, size_t fileCount) {
  for (size_t i = 0; i < fileCount; i += 1) {
    for (size_t j = i + 1; j < fileCount; j += 1) {
      if (files[j].size < files[i].size) {
        MothFile tmp = files[i];
        files[i] = files[j];
        files[j] = tmp;
      }
    }
  }
}

size_t applyUploadOptions(MothFile *files, size_t fileCount, const UploadOptions &options) {
  if (!uploadOptionsActive(options)) return fileCount;

  size_t kept = 0;
  for (size_t i = 0; i < fileCount; i += 1) {
    if (!uploadFileMatchesOptions(files[i], options)) continue;
    if (kept != i) files[kept] = files[i];
    kept += 1;
  }

  if (options.preferSmallest) {
    sortUploadFilesBySize(files, kept);
  }
  if (options.maxFiles > 0 && kept > options.maxFiles) {
    kept = options.maxFiles;
  }
  return kept;
}

UploadSummary runAudioMothUploadSession(long serverEpoch, bool forced) {
  return runAudioMothUploadSession(serverEpoch, forced, defaultUploadOptions());
}

UploadSummary runAudioMothUploadSession(long serverEpoch, bool forced, const UploadOptions &options) {
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
  size_t listedCount = fileCount;
  summary.filesSeen = (uint16_t)listedCount;
  if (uploadOptionsActive(options)) {
    fileCount = applyUploadOptions(files, fileCount, options);
    Serial.printf("Upload filter selected %u of %u listed file(s)\n",
                  (unsigned)fileCount, (unsigned)listedCount);
  }
  if (fileCount == 0) {
    closeBridgeSession();
    summary.code = UPLOAD_NO_FILES;
    summary.message = uploadOptionsActive(options) ? "no files matched upload filter" : "no files listed";
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
    Serial.printf("Allocating %u-byte server upload buffer: free_heap=%u max_block=%u\n",
                  SERVER_UPLOAD_CHUNK_BYTES, ESP.getFreeHeap(), ESP.getMaxAllocHeap());
    serverChunk = (uint8_t *)malloc(SERVER_UPLOAD_CHUNK_BYTES);
    if (!serverChunk) {
      Serial.printf("Could not allocate %u-byte upload buffer\n", SERVER_UPLOAD_CHUNK_BYTES);
      return false;
    }
    Serial.printf("Server upload buffer ready: free_heap=%u max_block=%u\n",
                  ESP.getFreeHeap(), ESP.getMaxAllocHeap());
  }

  uint32_t offset = session.resumeOffset;
  uint32_t transferStartMs = millis();
  uint32_t uartTransferMs = 0;
  uint32_t serverTransferMs = 0;
  uint32_t sdReadMs = 0;
  uint32_t serverProcessMs = 0;

#if MOTH_PIPE_FAST_ENABLED
  if (offset < file.size) {
    PipeSession pipe;
    bool pipeUnsupported = false;
    bool pipeStarted = bridgeStartPipeStream(file.path, offset, file.size - offset, pipe, pipeUnsupported);
    if (pipeStarted) {
      Serial.printf("GETPIPE transferring %lu bytes from offset %lu at %lu baud\n",
                    (unsigned long)pipe.totalBytes,
                    (unsigned long)pipe.startOffset,
                    (unsigned long)pipe.baud);

      bool pipeDone = false;
      while (offset < file.size && !pipeDone) {
        ChunkResult pipeChunk;
        bool pipeFatal = false;
        uint32_t pipeStartMs = millis();
        bool gotPipeChunk = bridgeReadPipeBlock(pipe, serverChunk, pipeChunk, pipeDone, pipeFatal);
        uint32_t pipeMs = millis() - pipeStartMs;
        uartTransferMs += pipeMs;

        if (!gotPipeChunk || !pipeChunk.ok || pipeChunk.offset != offset ||
            pipeChunk.length == 0 || pipeChunk.length > session.chunkSize) {
          Serial.printf("GETPIPE failed at offset %lu\n", (unsigned long)offset);
          if (!pipeDone) bridgeStopPipeStream();
          bridgeFailure = true;
          return false;
        }

        sdReadMs += pipeChunk.sdReadMs;
        Serial.printf("GETPIPE %lu bytes at offset %lu in %lu ms\n",
                      (unsigned long)pipeChunk.length,
                      (unsigned long)pipeChunk.offset,
                      (unsigned long)pipeMs);

        uint32_t serverStartMs = millis();
        uint32_t chunkServerProcessMs = 0;
        bool uploaded = serverUploadChunk(serverEpoch, session, serverChunk, offset, pipeChunk.length, chunkServerProcessMs);
        serverTransferMs += millis() - serverStartMs;
        serverProcessMs += chunkServerProcessMs;
        if (!uploaded) {
          Serial.printf("serverUploadChunk failed at offset %lu\n", (unsigned long)offset);
          if (!pipeDone) bridgeStopPipeStream();
          return false;
        }

        offset += pipeChunk.length;
        if ((offset % (256UL * 1024UL)) == 0 || offset >= file.size) {
          Serial.printf("Progress %s: %lu/%lu\n", file.path.c_str(), (unsigned long)offset, (unsigned long)file.size);
        }

        PowerState p = readPowerState();
        if (p.batteryV >= BATTERY_SENSE_INVALID_BELOW_V && p.batteryV < MIN_ACTIVE_BATTERY_V) {
          Serial.printf("Battery under load fell below emergency cutoff: %.3f V\n", p.batteryV);
          if (!pipeDone) bridgeStopPipeStream();
          return false;
        }

        if (!pipeDone && !bridgeContinuePipeStream(pipe)) {
          Serial.printf("GETPIPE NEXT failed at offset %lu\n", (unsigned long)offset);
          bridgeFailure = true;
          return false;
        }
      }

      if (offset < file.size) {
        Serial.printf("GETPIPE ended early at offset %lu of %lu\n", (unsigned long)offset, (unsigned long)file.size);
        bridgeFailure = true;
        return false;
      }
    } else if (!pipeUnsupported) {
      Serial.printf("GETPIPE did not start at offset %lu\n", (unsigned long)offset);
      bridgeFailure = true;
      return false;
    }
  }
#endif

#if MOTH_ALLOW_115200_GET_FALLBACK
  while (offset < file.size) {
    uint32_t remaining = file.size - offset;
    uint32_t batchBytes = remaining > session.chunkSize ? session.chunkSize : remaining;
    uint32_t filled = 0;

    while (filled < batchBytes) {
      uint32_t uartRemaining = batchBytes - filled;
      uint32_t requestBytes = uartRemaining > MOTH_CHUNK_BYTES ? MOTH_CHUNK_BYTES : uartRemaining;
      uint32_t uartOffset = offset + filled;

      ChunkResult chunk;
      uint32_t uartStartMs = millis();
      bool gotChunk = bridgeGetChunk(file.path, uartOffset, requestBytes, chunk);
      uartTransferMs += millis() - uartStartMs;
      if (!gotChunk || !chunk.ok || chunk.offset != uartOffset || chunk.length == 0 || chunk.length != requestBytes) {
        Serial.printf("GET failed at offset %lu\n", (unsigned long)uartOffset);
        bridgeFailure = true;
        return false;
      }

      memcpy(serverChunk + filled, mothChunk, chunk.length);
      sdReadMs += chunk.sdReadMs;
      filled += chunk.length;
    }

    uint32_t serverStartMs = millis();
    uint32_t chunkServerProcessMs = 0;
    bool uploaded = serverUploadChunk(serverEpoch, session, serverChunk, offset, batchBytes, chunkServerProcessMs);
    serverTransferMs += millis() - serverStartMs;
    serverProcessMs += chunkServerProcessMs;
    if (!uploaded) {
      Serial.printf("serverUploadChunk failed at offset %lu\n", (unsigned long)offset);
      return false;
    }

    offset += batchBytes;
    if ((offset % (256UL * 1024UL)) == 0 || offset >= file.size) {
      Serial.printf("Progress %s: %lu/%lu\n", file.path.c_str(), (unsigned long)offset, (unsigned long)file.size);
    }

    PowerState p = readPowerState();
    if (p.batteryV >= BATTERY_SENSE_INVALID_BELOW_V && p.batteryV < MIN_ACTIVE_BATTERY_V) {
      Serial.printf("Battery under load fell below emergency cutoff: %.3f V\n", p.batteryV);
      return false;
    }
  }
#else
  if (offset < file.size) {
    Serial.printf("GETPIPE required for production upload; rejecting 115200-baud GET fallback at offset %lu\n",
                  (unsigned long)offset);
    bridgeFailure = true;
    return false;
  }
#endif

  closeUploadHttpClient();
  if (serverChunk) {
    free(serverChunk);
    serverChunk = nullptr;
    Serial.printf("Released %u-byte upload buffer before finalizing file\n", SERVER_UPLOAD_CHUNK_BYTES);
  }

  if (!serverFinishFile(serverEpoch, session)) {
    Serial.println("serverFinishFile failed");
    return false;
  }

  uint32_t elapsedMs = millis() - transferStartMs;
  uint32_t transferredBytes = file.size - session.resumeOffset;
  float kibPerSecond = elapsedMs > 0 ? ((float)transferredBytes * 1000.0f) / (1024.0f * (float)elapsedMs) : 0.0f;
  float uartKibPerSecond = uartTransferMs > 0 ? ((float)transferredBytes * 1000.0f) / (1024.0f * (float)uartTransferMs) : 0.0f;
  float serverKibPerSecond = serverTransferMs > 0 ? ((float)transferredBytes * 1000.0f) / (1024.0f * (float)serverTransferMs) : 0.0f;
  float sdKibPerSecond = sdReadMs > 0 ? ((float)transferredBytes * 1000.0f) / (1024.0f * (float)sdReadMs) : 0.0f;
  Serial.printf("Completed %s in %.1f s: end_to_end=%.1f KiB/s sd=%.1f KiB/s uart=%.1f KiB/s network=%.1f KiB/s sd_ms=%lu uart_ms=%lu network_ms=%lu server_process_ms=%lu\n",
                file.path.c_str(), elapsedMs / 1000.0f, kibPerSecond,
                sdKibPerSecond, uartKibPerSecond, serverKibPerSecond,
                (unsigned long)sdReadMs, (unsigned long)uartTransferMs,
                (unsigned long)serverTransferMs, (unsigned long)serverProcessMs);

  return true;
}
