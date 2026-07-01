void initMothBridge() {
  pinMode(PIN_MOTH_REQ, OUTPUT);
  digitalWrite(PIN_MOTH_REQ, MOTH_ASSERT_REQ_AT_BOOT ? HIGH : LOW);

  pinMode(PIN_MOTH_BUSY, INPUT_PULLDOWN);

  pinMode(PIN_MOTH_UART_RX, INPUT_PULLUP);
  pinMode(PIN_MOTH_UART_TX, INPUT_PULLUP);

  /* Leave the configured UART pins owned by UART2 after begin(). Calling pinMode() on
     either UART pin after this can detach the ESP32 pin matrix from Serial2. */
  MothSerial.setRxBufferSize(MOTH_UART_RX_BUFFER_BYTES);
  MothSerial.begin(MOTH_UART_BAUD, SERIAL_8N1, PIN_MOTH_UART_RX, PIN_MOTH_UART_TX);
  while (MothSerial.available()) MothSerial.read();
}

uint32_t bridgeRawBytesRead = 0;
uint32_t bridgeLinesRead = 0;
bool bridgeFastBaudActive = false;
bool bridgeSessionBaudActive = false;
uint32_t bridgeCurrentBaud = MOTH_UART_BAUD;
uint32_t bridgeRejectedSessionBauds[3] = {0, 0, 0};
uint8_t bridgeRejectedSessionBaudCount = 0;

void bridgeResetStats() {
  bridgeRawBytesRead = 0;
  bridgeLinesRead = 0;
}

bool mothBusy() {
  return digitalRead(PIN_MOTH_BUSY) == HIGH;
}

void mothRequest(bool state) {
  digitalWrite(PIN_MOTH_REQ, state ? HIGH : LOW);
}

void bridgeFlushInput() {
  while (MothSerial.available()) MothSerial.read();
}

void bridgeDrainInputQuiet(uint32_t quietMs, uint32_t maxMs) {
  uint32_t start = millis();
  uint32_t quietStart = millis();
  while (millis() - start < maxMs) {
    bool sawByte = false;
    while (MothSerial.available()) {
      MothSerial.read();
      sawByte = true;
    }
    if (sawByte) quietStart = millis();
    if (millis() - quietStart >= quietMs) return;
    delay(1);
  }
}

void bridgeRestartUart(uint32_t baud, bool clearInput) {
  MothSerial.flush();
  MothSerial.end();
  delay(2);
  MothSerial.setRxBufferSize(MOTH_UART_RX_BUFFER_BYTES);
  MothSerial.begin(baud, SERIAL_8N1, PIN_MOTH_UART_RX, PIN_MOTH_UART_TX);
  bridgeCurrentBaud = baud;
  if (clearInput) bridgeFlushInput();
}

void bridgeSendLine(const String &line) {
#if DEBUG_BRIDGE_LINES
  Serial.print("MOTH << ");
  Serial.println(line);
#endif
  MothSerial.print(line);
  MothSerial.print('\n');
  MothSerial.flush();
}

bool bridgeReadLine(String &line, uint32_t timeoutMs) {
  line = "";
  uint32_t start = millis();
  while (millis() - start < timeoutMs) {
    while (MothSerial.available()) {
      char c = (char)MothSerial.read();
      bridgeRawBytesRead += 1;
      if (c == '\r') continue;
      if (c == '\n') {
        if (line.length() == 0) continue;
        bridgeLinesRead += 1;
#if DEBUG_BRIDGE_LINES
        Serial.print("MOTH >> ");
        Serial.println(line);
#endif
        return true;
      }
      if (line.length() < 220) line += c;
    }
    delay(1);
  }
  return false;
}

bool bridgeReadBytes(uint8_t *dest, uint32_t length, uint32_t timeoutMs) {
  uint32_t got = 0;
  uint32_t start = millis();
  while (got < length && millis() - start < timeoutMs) {
    while (MothSerial.available() && got < length) {
      dest[got++] = (uint8_t)MothSerial.read();
      bridgeRawBytesRead += 1;
      start = millis();
    }
    delay(1);
  }
  return got == length;
}

bool bridgeReadMagic(const uint8_t *magic, uint32_t magicLength, uint32_t timeoutMs) {
  uint32_t matched = 0;
  uint32_t start = millis();

  while (millis() - start < timeoutMs) {
    while (MothSerial.available()) {
      uint8_t byte = (uint8_t)MothSerial.read();
      bridgeRawBytesRead += 1;
      if (byte == magic[matched]) {
        matched += 1;
        if (matched == magicLength) return true;
      } else {
        matched = byte == magic[0] ? 1 : 0;
      }
    }
    delay(1);
  }

  return false;
}

bool bridgeReadFastMagic(uint32_t timeoutMs) {
  static const uint8_t magic[] = {0xA5, 0x5A, 0xC3, 0x3C};
  return bridgeReadMagic(magic, sizeof(magic), timeoutMs);
}

bool bridgeIsAsyncLine(const String &line) {
  return line == "OK BRIDGE_READY" || line == "OK FAST_READY" || line == "OK PONG";
}

bool bridgeReadExpectedLine(const char *expectedPrefix, String &lineOut, uint32_t timeoutMs) {
  lineOut = "";
  uint32_t start = millis();
  while (millis() - start < timeoutMs) {
    String line;
    uint32_t remaining = timeoutMs - (millis() - start);
    uint32_t slice = remaining > 1000 ? 1000 : remaining;
    if (slice == 0) break;
    if (!bridgeReadLine(line, slice)) continue;

    if (line.startsWith(expectedPrefix)) {
      lineOut = line;
      return true;
    }

    if (bridgeIsAsyncLine(line)) continue;
    if (line.startsWith("ERR") || line == "OK BRIDGE_SLEEP") {
      lineOut = line;
      return false;
    }

    lineOut = line;
    return false;
  }
  return false;
}

bool bridgeReadExpectedLineIgnoringNoise(const char *expectedPrefix, String &lineOut, uint32_t timeoutMs) {
  lineOut = "";
  uint32_t start = millis();
  while (millis() - start < timeoutMs) {
    String line;
    uint32_t remaining = timeoutMs - (millis() - start);
    uint32_t slice = remaining > 1000 ? 1000 : remaining;
    if (slice == 0) break;
    if (!bridgeReadLine(line, slice)) continue;

    if (line.startsWith(expectedPrefix)) {
      lineOut = line;
      return true;
    }
    if (line.startsWith("ERR") || line == "OK BRIDGE_SLEEP") {
      lineOut = line;
      return false;
    }
  }
  return false;
}

void bridgeReturnToDefaultAfterStream() {
  MothSerial.updateBaudRate(MOTH_UART_BAUD);
  bridgeCurrentBaud = MOTH_UART_BAUD;
  delay(20);
}

bool bridgeProbeDefaultControl(const char *context) {
  String lastLine;
  bool sawLine = false;
  for (uint8_t attempt = 0; attempt < 4; attempt += 1) {
    bridgeSendLine("PING");
    uint32_t start = millis();
    while (millis() - start < 1500) {
      String line;
      uint32_t remaining = 1500 - (millis() - start);
      uint32_t slice = remaining > 500 ? 500 : remaining;
      if (slice == 0) break;
      if (!bridgeReadLine(line, slice)) continue;

      sawLine = true;
      lastLine = line;
      if (line == "OK PONG" || line.startsWith("OK PONG ")) return true;
      if (line == "OK BRIDGE_SLEEP") break;
    }
    delay(100);
  }
  if (sawLine) {
    Serial.printf("%s control resync failed; last line='%s'\n", context, lastLine.c_str());
  } else {
    Serial.printf("%s control resync failed; no UART lines received\n", context);
  }
  return false;
}

bool bridgeSessionBaudRejected(uint32_t baud) {
  for (uint8_t i = 0; i < bridgeRejectedSessionBaudCount; i += 1) {
    if (bridgeRejectedSessionBauds[i] == baud) return true;
  }
  return false;
}

void bridgeRejectSessionBaud(uint32_t baud) {
  if (baud == MOTH_UART_BAUD || bridgeSessionBaudRejected(baud)) return;
  if (bridgeRejectedSessionBaudCount >= 3) return;
  bridgeRejectedSessionBauds[bridgeRejectedSessionBaudCount++] = baud;
}

bool bridgeReopenDefaultSession() {
  mothRequest(false);
  bridgeSessionBaudActive = false;
  bridgeFastBaudActive = false;
  bridgeRestartUart(MOTH_UART_BAUD, true);
  delay(MOTH_SESSION_RESET_MS);
  mothRequest(true);

  if (!bridgeWaitReady(MOTH_READY_TIMEOUT_MS)) {
    Serial.println("AudioMoth did not reopen at default baud after high-baud probe");
    return false;
  }
  if (!bridgePing()) {
    Serial.println("AudioMoth default-baud PING failed after high-baud probe");
    return false;
  }
  return true;
}

bool bridgeWaitReady(uint32_t timeoutMs) {
  bridgeResetStats();
  String line;
  uint32_t start = millis();
  uint32_t lastPingMs = 0;
  uint32_t pingsSent = 0;
  bool busyLowSeen = !mothBusy();

  while (millis() - start < timeoutMs) {
    if (!mothBusy()) busyLowSeen = true;

    if (bridgeReadLine(line, 500)) {
      if (line == "OK BRIDGE_READY" || line == "OK PONG") {
        Serial.printf("Bridge READY after %lu ms; pings=%lu rx_bytes=%lu rx_lines=%lu busy_low_seen=%d busy_now=%d esp_req=%d\n",
                      (unsigned long)(millis() - start),
                      (unsigned long)pingsSent,
                      (unsigned long)bridgeRawBytesRead,
                      (unsigned long)bridgeLinesRead,
                      busyLowSeen ? 1 : 0,
                      mothBusy() ? 1 : 0,
                      digitalRead(PIN_MOTH_REQ));
        return true;
      }
      if (line.startsWith("ERR")) {
        Serial.printf("Bridge READY failed on AudioMoth error after %lu ms; pings=%lu rx_bytes=%lu rx_lines=%lu busy_low_seen=%d busy_now=%d esp_req=%d\n",
                      (unsigned long)(millis() - start),
                      (unsigned long)pingsSent,
                      (unsigned long)bridgeRawBytesRead,
                      (unsigned long)bridgeLinesRead,
                      busyLowSeen ? 1 : 0,
                      mothBusy() ? 1 : 0,
                      digitalRead(PIN_MOTH_REQ));
        return false;
      }
    }

    uint32_t elapsed = millis() - start;
    if (elapsed - lastPingMs >= 2000) {
      bridgeSendLine("PING");
      lastPingMs = elapsed;
      pingsSent += 1;
    }
  }

  Serial.printf("Bridge READY timeout after %lu ms; pings=%lu rx_bytes=%lu rx_lines=%lu busy_low_seen=%d busy_now=%d esp_req=%d\n",
                (unsigned long)(millis() - start),
                (unsigned long)pingsSent,
                (unsigned long)bridgeRawBytesRead,
                (unsigned long)bridgeLinesRead,
                busyLowSeen ? 1 : 0,
                mothBusy() ? 1 : 0,
                digitalRead(PIN_MOTH_REQ));
  return false;
}

bool bridgeExpectResponse(const String &cmd, const char *expectedPrefix, String *responseOut = nullptr) {
  bridgeSendLine(cmd);
  String line;
  if (!bridgeReadExpectedLine(expectedPrefix, line, MOTH_LINE_TIMEOUT_MS)) return false;
  if (responseOut) *responseOut = line;
  return line.startsWith(expectedPrefix);
}

bool bridgePing() {
  for (uint8_t attempt = 1; attempt <= 4; attempt++) {
    bridgeSendLine("PING");
    String line;
    if (bridgeReadExpectedLine("OK PONG", line, 1000)) return true;
    delay(75);
  }
  return false;
}

bool bridgeTrySessionBaud(uint32_t baud) {
  if (baud == MOTH_UART_BAUD) return true;
  if (bridgeSessionBaudRejected(baud)) return false;

  Serial.printf("Trying AudioMoth session baud %lu\n", (unsigned long)baud);
  bridgeSendLine("BAUD " + String(baud));

  String line;
  if (!bridgeReadExpectedLine("OK BAUD", line, MOTH_LINE_TIMEOUT_MS)) {
    Serial.printf("AudioMoth BAUD %lu was not accepted: %s\n",
                  (unsigned long)baud, line.length() ? line.c_str() : "no response");
    bridgeRejectSessionBaud(baud);
    return false;
  }

  unsigned long acceptedBaud = 0;
  if (sscanf(line.c_str(), "OK BAUD %lu", &acceptedBaud) != 1 || acceptedBaud != baud) {
    Serial.printf("AudioMoth BAUD response mismatch: %s\n", line.c_str());
    bridgeRejectSessionBaud(baud);
    return false;
  }

  bridgeRestartUart(baud, true);
  if (!bridgeReadExpectedLineIgnoringNoise("OK FAST_READY", line, MOTH_FAST_DONE_TIMEOUT_MS)) {
    Serial.printf("AudioMoth session baud %lu did not produce FAST_READY: %s\n",
                  (unsigned long)baud, line.length() ? line.c_str() : "no response");
    bridgeRejectSessionBaud(baud);
    return false;
  }

  if (!bridgePing()) {
    Serial.printf("AudioMoth session baud %lu failed PING proof\n", (unsigned long)baud);
    bridgeRejectSessionBaud(baud);
    return false;
  }

  bridgeSessionBaudActive = true;
  bridgeFastBaudActive = false;
  Serial.printf("AudioMoth session baud active at %lu; using %u-byte UART DATA chunks\n",
                (unsigned long)baud, MOTH_CHUNK_BYTES);
  return true;
}

bool bridgeEnableFastBaud() {
  bridgeFastBaudActive = false;
  bridgeSessionBaudActive = false;

#if MOTH_SESSION_FAST_ENABLED
  const uint32_t sessionCandidates[] = {
    MOTH_SESSION_FAST_BAUD,
    MOTH_SESSION_RETRY_BAUD_1,
    MOTH_SESSION_RETRY_BAUD_2
  };

  for (uint8_t i = 0; i < sizeof(sessionCandidates) / sizeof(sessionCandidates[0]); i += 1) {
    uint32_t baud = sessionCandidates[i];
    if (baud <= MOTH_UART_BAUD || bridgeSessionBaudRejected(baud)) continue;
    if (bridgeTrySessionBaud(baud)) return true;

    Serial.printf("Falling back from failed session baud %lu\n", (unsigned long)baud);
    if (!bridgeReopenDefaultSession()) return false;
  }
#endif

  if (MOTH_UART_FAST_BAUD == MOTH_UART_BAUD) return true;

  bridgeSendLine("FASTCAP " + String(MOTH_UART_FAST_BAUD));
  String line;
  if (!bridgeReadExpectedLine("OK FASTCAP", line, MOTH_LINE_TIMEOUT_MS)) {
    Serial.printf("AudioMoth fast payload mode is unavailable; continuing at %u baud (%s)\n",
                  MOTH_UART_BAUD, line.length() ? line.c_str() : "no response");
    bridgeFlushInput();
    return true;
  }

  unsigned long negotiatedBaud = 0;
  unsigned int maxChunk = 0;
  if (sscanf(line.c_str(), "OK FASTCAP %lu %u", &negotiatedBaud, &maxChunk) != 2 ||
      negotiatedBaud != MOTH_UART_FAST_BAUD || maxChunk < MOTH_CHUNK_BYTES) {
    Serial.printf("AudioMoth returned an invalid fast payload capability: %s\n", line.c_str());
    return true;
  }

  bridgeFastBaudActive = true;
  Serial.printf("AudioMoth fast payload mode armed at %u baud with %u-byte UART chunks\n",
                MOTH_UART_FAST_BAUD, MOTH_CHUNK_BYTES);
  return true;
}

uint32_t bridgeTransferChunkBytes() {
  return (bridgeSessionBaudActive || bridgeFastBaudActive) ? MOTH_CHUNK_BYTES : MOTH_LEGACY_CHUNK_BYTES;
}

bool bridgeSetTime(uint32_t epochUtc, uint32_t milliseconds) {
  String cmd = "TIME " + String(epochUtc) + " " + String(milliseconds);
  String line;
  return bridgeExpectResponse(cmd, "OK TIME", &line);
}

bool bridgeStatus(String &statusOut) {
  return bridgeExpectResponse("STATUS", "OK STATUS", &statusOut);
}

bool bridgePathCharAllowed(char c) {
  return (c >= 'A' && c <= 'Z') ||
         (c >= 'a' && c <= 'z') ||
         (c >= '0' && c <= '9') ||
         c == '_' || c == '-' || c == '.' || c == '/';
}

bool bridgeIsConfigTxtPath(const String &path) {
  int slash = path.lastIndexOf('/');
  String name = slash >= 0 ? path.substring(slash + 1) : path;
  name.toLowerCase();
  return name == "config.txt";
}

bool bridgePathCanRoundTrip(const String &path) {
  if (path.length() == 0 || path.length() >= 96) return false;
  if (path[0] == '/' || path[0] == '\\') return false;
  if (path.indexOf("..") >= 0) return false;
  if (bridgeIsConfigTxtPath(path)) return false;

  for (size_t i = 0; i < path.length(); i += 1) {
    if (!bridgePathCharAllowed(path[i])) return false;
  }

  return true;
}

bool bridgeParseFileLine(const String &line, String &pathOut, uint32_t &sizeOut) {
  pathOut = "";
  sizeOut = 0;
  if (!line.startsWith("FILE ")) return false;

  int lastSpace = line.lastIndexOf(' ');
  if (lastSpace <= 5 || lastSpace >= (int)line.length() - 1) return false;

  String sizeText = line.substring(lastSpace + 1);
  for (size_t i = 0; i < sizeText.length(); i += 1) {
    if (sizeText[i] < '0' || sizeText[i] > '9') return false;
  }

  pathOut = line.substring(5, lastSpace);
  sizeOut = (uint32_t)strtoul(sizeText.c_str(), nullptr, 10);
  return true;
}

bool bridgeParseSdLine(const String &line, MothSdInfo &sdOut) {
  if (!line.startsWith("SD ")) return false;

  unsigned long totalKb = 0;
  unsigned long freeKb = 0;
  int matched = sscanf(line.c_str(), "SD total_kb=%lu free_kb=%lu", &totalKb, &freeKb);
  if (matched != 2) return false;

  sdOut.valid = true;
  sdOut.totalKb = (uint32_t)totalKb;
  sdOut.freeKb = (uint32_t)freeKb;
  return true;
}

bool bridgeList(MothFile *files, size_t maxFiles, size_t &countOut, MothSdInfo *sdInfo) {
  countOut = 0;
  if (sdInfo) {
    sdInfo->valid = false;
    sdInfo->totalKb = 0;
    sdInfo->freeKb = 0;
  }
  bridgeSendLine("LIST");

  uint32_t start = millis();
  while (millis() - start < MOTH_LIST_TIMEOUT_MS) {
    String line;
    if (!bridgeReadLine(line, 1000)) continue;

    if (line == "END") return true;
    if (line.startsWith("ERR")) {
      Serial.printf("AudioMoth LIST error: %s\n", line.c_str());
      return false;
    }
    if (bridgeIsAsyncLine(line)) continue;
    if (line == "OK BRIDGE_SLEEP") {
      Serial.println("AudioMoth LIST ended because the bridge service slept");
      return false;
    }

    if (line.startsWith("SD ")) {
      MothSdInfo parsed = {false, 0, 0};
      if (bridgeParseSdLine(line, parsed)) {
        if (sdInfo) *sdInfo = parsed;
        Serial.printf("AudioMoth SD: free=%lu MB total=%lu MB\n",
                      (unsigned long)(parsed.freeKb / 1024UL),
                      (unsigned long)(parsed.totalKb / 1024UL));
      } else {
        Serial.printf("Skipping malformed AudioMoth SD line: %s\n", line.c_str());
      }
      continue;
    }

    if (line.startsWith("INFO ")) {
      Serial.print("AudioMoth LIST ");
      Serial.println(line);
      continue;
    }

    if (line.startsWith("FILE ")) {
      String path;
      uint32_t size = 0;
      if (!bridgeParseFileLine(line, path, size)) {
        Serial.printf("Skipping malformed AudioMoth FILE line: %s\n", line.c_str());
        continue;
      }

      if (!bridgePathCanRoundTrip(path)) {
        Serial.printf("Skipping unsafe AudioMoth path: %s\n", path.c_str());
        continue;
      }

      if (countOut < maxFiles) {
        files[countOut].path = path;
        files[countOut].size = size;
        files[countOut].localFileId = 0;
        countOut += 1;
        Serial.printf("AudioMoth FILE %s %lu\n", path.c_str(), (unsigned long)size);
      } else {
        Serial.printf("AudioMoth LIST max file count reached; ignoring %s\n", path.c_str());
      }
      continue;
    }

    Serial.printf("AudioMoth LIST ignored line: %s\n", line.c_str());
  }

  Serial.printf("AudioMoth LIST timeout after %lu ms; files=%u rx_bytes=%lu rx_lines=%lu\n",
                (unsigned long)MOTH_LIST_TIMEOUT_MS,
                (unsigned int)countOut,
                (unsigned long)bridgeRawBytesRead,
                (unsigned long)bridgeLinesRead);
  return false;
}

uint16_t bridgeReadUInt16LE(const uint8_t *data) {
  return (uint16_t)data[0] | ((uint16_t)data[1] << 8);
}

uint32_t bridgeReadUInt32LE(const uint8_t *data) {
  return (uint32_t)data[0] |
         ((uint32_t)data[1] << 8) |
         ((uint32_t)data[2] << 16) |
         ((uint32_t)data[3] << 24);
}

bool bridgeFastStreamSupported = true;

bool bridgeRunTestStream(uint32_t requestedBytes, uint32_t baud, uint32_t &receivedOut, uint32_t &elapsedMsOut, uint32_t &crcOut) {
  receivedOut = 0;
  elapsedMsOut = 0;
  crcOut = 0;

  if (requestedBytes == 0) requestedBytes = MOTH_TEST_STREAM_BYTES;
  if (requestedBytes > MOTH_TEST_STREAM_BYTES) requestedBytes = MOTH_TEST_STREAM_BYTES;
  if (baud == 0) baud = MOTH_STREAM_FAST_BAUD;
  if (baud == MOTH_UART_BAUD) return false;
  if (bridgeCurrentBaud != MOTH_UART_BAUD || bridgeSessionBaudActive || bridgeFastBaudActive) {
    return false;
  }

  bridgeFlushInput();
  bridgeSendLine("TESTSTREAM " + String(requestedBytes) + " " + String(baud));

  String line;
  if (!bridgeReadExpectedLine("TESTSTREAM ", line, MOTH_DATA_HEADER_TIMEOUT_MS) || !line.startsWith("TESTSTREAM ")) {
    if (line.length()) {
      Serial.printf("TESTSTREAM header failed: %s\n", line.c_str());
    } else {
      Serial.println("TESTSTREAM header timed out");
    }
    bridgeFlushInput();
    return false;
  }

  unsigned long streamLength = 0;
  unsigned int frameMax = 0;
  unsigned long streamBaud = 0;
  if (sscanf(line.c_str(), "TESTSTREAM %lu %u %lu", &streamLength, &frameMax, &streamBaud) != 3) {
    Serial.printf("TESTSTREAM malformed header: %s\n", line.c_str());
    return false;
  }
  if (streamLength == 0 || streamLength > requestedBytes || frameMax == 0 ||
      frameMax > MOTH_CHUNK_BYTES || streamBaud != baud) {
    Serial.printf("TESTSTREAM header mismatch: %s\n", line.c_str());
    return false;
  }

  uint32_t startMs = millis();
  MothSerial.updateBaudRate(baud);

  static const uint8_t streamMagic[] = {0xA5, 0x5A, 0xD7, 0x7D};
  uint8_t frameHeader[14];
  uint32_t received = 0;
  uint32_t combinedCrc = 0;

  while (received < streamLength) {
    if (!bridgeReadMagic(streamMagic, sizeof(streamMagic), MOTH_STREAM_FRAME_TIMEOUT_MS)) {
      Serial.printf("TESTSTREAM frame magic timeout received %lu/%lu\n",
                    (unsigned long)received, (unsigned long)streamLength);
      bridgeRestartUart(MOTH_UART_BAUD, true);
      return false;
    }

    if (!bridgeReadBytes(frameHeader, sizeof(frameHeader), MOTH_STREAM_FRAME_TIMEOUT_MS)) {
      Serial.printf("TESTSTREAM frame header timeout received %lu/%lu\n",
                    (unsigned long)received, (unsigned long)streamLength);
      bridgeRestartUart(MOTH_UART_BAUD, true);
      return false;
    }

    uint32_t frameOffset = bridgeReadUInt32LE(frameHeader);
    uint16_t frameLength = bridgeReadUInt16LE(frameHeader + 4);
    uint32_t frameCrc = bridgeReadUInt32LE(frameHeader + 6);
    if (frameOffset != received || frameLength == 0 || frameLength > frameMax || received + frameLength > streamLength) {
      Serial.printf("TESTSTREAM metadata mismatch: frame_offset=%lu expected=%lu len=%u total=%lu\n",
                    (unsigned long)frameOffset, (unsigned long)received, frameLength,
                    (unsigned long)streamLength);
      bridgeRestartUart(MOTH_UART_BAUD, true);
      return false;
    }

    if (!bridgeReadBytes(mothChunk, frameLength, MOTH_BINARY_TIMEOUT_MS)) {
      Serial.printf("TESTSTREAM payload timeout at offset %lu length %u\n",
                    (unsigned long)frameOffset, frameLength);
      bridgeRestartUart(MOTH_UART_BAUD, true);
      return false;
    }

    uint32_t localCrc = crc32Update(0, mothChunk, frameLength);
    if (localCrc != frameCrc) {
      Serial.printf("TESTSTREAM CRC mismatch at offset %lu: local=%08lX moth=%08lX\n",
                    (unsigned long)frameOffset, (unsigned long)localCrc, (unsigned long)frameCrc);
      bridgeRestartUart(MOTH_UART_BAUD, true);
      return false;
    }

    uint16_t headSamples = frameLength < 8 ? frameLength : 8;
    for (uint16_t i = 0; i < headSamples; i += 1) {
      uint8_t expected = (uint8_t)((frameOffset + i) & 0xFFU);
      if (mothChunk[i] != expected) {
        Serial.printf("TESTSTREAM payload pattern mismatch at offset %lu: got=%u expected=%u\n",
                      (unsigned long)(frameOffset + i), mothChunk[i], expected);
        bridgeRestartUart(MOTH_UART_BAUD, true);
        return false;
      }
    }
    uint16_t tailStart = frameLength > 16 ? frameLength - 8 : headSamples;
    for (uint16_t i = tailStart; i < frameLength; i += 1) {
      uint8_t expected = (uint8_t)((frameOffset + i) & 0xFFU);
      if (mothChunk[i] != expected) {
        Serial.printf("TESTSTREAM payload pattern mismatch at offset %lu: got=%u expected=%u\n",
                      (unsigned long)(frameOffset + i), mothChunk[i], expected);
        bridgeRestartUart(MOTH_UART_BAUD, true);
        return false;
      }
    }

    combinedCrc = crc32Update(combinedCrc, mothChunk, frameLength);
    received += frameLength;
  }

  bridgeReturnToDefaultAfterStream();
  elapsedMsOut = millis() - startMs;

  String doneLine;
  if (!bridgeReadExpectedLine("OK TESTSTREAM", doneLine, 150)) {
    if (doneLine.startsWith("ERR") || doneLine == "OK BRIDGE_SLEEP") {
      Serial.printf("TESTSTREAM completion error: %s\n", doneLine.c_str());
      return false;
    }
    Serial.printf("TESTSTREAM completion not observed; validated %lu bytes by frame CRC\n",
                  (unsigned long)received);
  }
  if (!bridgeProbeDefaultControl("TESTSTREAM")) {
    return false;
  }

  receivedOut = received;
  crcOut = combinedCrc;
  return received == streamLength;
}

bool bridgeGetStreamBlock(const String &path, uint32_t offset, uint32_t requestedBytes, uint8_t *dest, ChunkResult &result, bool &fatalOut) {
  result.ok = false;
  result.path = path;
  result.offset = offset;
  result.length = 0;
  result.crc = 0;
  result.sdReadMs = 0;
  fatalOut = false;

#if !MOTH_STREAM_FAST_ENABLED
  return false;
#endif

  if (!bridgeFastStreamSupported || !dest || requestedBytes == 0 || requestedBytes > SERVER_UPLOAD_CHUNK_BYTES) {
    return false;
  }
  if (bridgeCurrentBaud != MOTH_UART_BAUD || bridgeSessionBaudActive || bridgeFastBaudActive) {
    return false;
  }

  if (!bridgeProbeDefaultControl("GETSTREAM preflight")) {
    fatalOut = true;
    return false;
  }
  bridgeDrainInputQuiet(25, 250);
  bridgeSendLine("GETSTREAM " + path + " " + String(offset) + " " + String(requestedBytes) + " " + String(MOTH_STREAM_FAST_BAUD));

  String line;
  if (!bridgeReadExpectedLineIgnoringNoise("STREAM ", line, MOTH_DATA_HEADER_TIMEOUT_MS) || !line.startsWith("STREAM ")) {
    if (line.startsWith("ERR CMD") || line.startsWith("ERR ARG")) {
      Serial.printf("AudioMoth GETSTREAM is unavailable: %s\n", line.c_str());
      bridgeFastStreamSupported = false;
    } else if (line.length()) {
      Serial.printf("GETSTREAM header failed at offset %lu: %s\n", (unsigned long)offset, line.c_str());
    } else {
      Serial.printf("GETSTREAM header timed out at offset %lu\n", (unsigned long)offset);
    }
    bridgeFlushInput();
    return false;
  }

  char parsedPath[128] = {0};
  unsigned long parsedOffset = 0;
  unsigned long streamLength = 0;
  unsigned int frameMax = 0;
  unsigned long streamBaud = 0;
  if (sscanf(line.c_str(), "STREAM %127s %lu %lu %u %lu", parsedPath, &parsedOffset, &streamLength, &frameMax, &streamBaud) != 5) {
    Serial.printf("GETSTREAM malformed header: %s\n", line.c_str());
    fatalOut = true;
    return false;
  }
  if (String(parsedPath) != path || parsedOffset != offset || streamLength == 0 ||
      streamLength > requestedBytes || frameMax == 0 || frameMax > MOTH_CHUNK_BYTES ||
      streamBaud != MOTH_STREAM_FAST_BAUD) {
    Serial.printf("GETSTREAM header mismatch: %s\n", line.c_str());
    fatalOut = true;
    return false;
  }

  MothSerial.updateBaudRate(MOTH_STREAM_FAST_BAUD);

  static const uint8_t streamMagic[] = {0xA5, 0x5A, 0xD7, 0x7D};
  uint32_t received = 0;
  uint32_t combinedCrc = 0;
  uint32_t totalSdReadMs = 0;
  uint8_t frameHeader[14];

  while (received < streamLength) {
    if (!bridgeReadMagic(streamMagic, sizeof(streamMagic), MOTH_STREAM_FRAME_TIMEOUT_MS)) {
      Serial.printf("GETSTREAM frame magic timeout at offset %lu received %lu/%lu\n",
                    (unsigned long)offset, (unsigned long)received, (unsigned long)streamLength);
      fatalOut = true;
      bridgeRestartUart(MOTH_UART_BAUD, true);
      return false;
    }

    if (!bridgeReadBytes(frameHeader, sizeof(frameHeader), MOTH_STREAM_FRAME_TIMEOUT_MS)) {
      Serial.printf("GETSTREAM frame header timeout at offset %lu received %lu/%lu\n",
                    (unsigned long)offset, (unsigned long)received, (unsigned long)streamLength);
      fatalOut = true;
      bridgeRestartUart(MOTH_UART_BAUD, true);
      return false;
    }

    uint32_t frameOffset = bridgeReadUInt32LE(frameHeader);
    uint16_t frameLength = bridgeReadUInt16LE(frameHeader + 4);
    uint32_t frameCrc = bridgeReadUInt32LE(frameHeader + 6);
    uint32_t frameSdMs = bridgeReadUInt32LE(frameHeader + 10);
    uint32_t expectedOffset = offset + received;

    if (frameOffset != expectedOffset || frameLength == 0 || frameLength > frameMax || received + frameLength > streamLength) {
      Serial.printf("GETSTREAM frame metadata mismatch: frame_offset=%lu expected=%lu len=%u received=%lu total=%lu\n",
                    (unsigned long)frameOffset, (unsigned long)expectedOffset, frameLength,
                    (unsigned long)received, (unsigned long)streamLength);
      fatalOut = true;
      bridgeRestartUart(MOTH_UART_BAUD, true);
      return false;
    }

    if (!bridgeReadBytes(dest + received, frameLength, MOTH_BINARY_TIMEOUT_MS)) {
      Serial.printf("GETSTREAM payload timeout at offset %lu length %u\n",
                    (unsigned long)frameOffset, frameLength);
      fatalOut = true;
      bridgeRestartUart(MOTH_UART_BAUD, true);
      return false;
    }

    uint32_t localCrc = crc32Update(0, dest + received, frameLength);
    if (localCrc != frameCrc) {
      Serial.printf("GETSTREAM CRC mismatch at offset %lu: local=%08lX moth=%08lX\n",
                    (unsigned long)frameOffset, (unsigned long)localCrc, (unsigned long)frameCrc);
      fatalOut = true;
      bridgeRestartUart(MOTH_UART_BAUD, true);
      return false;
    }

    combinedCrc = crc32Update(combinedCrc, dest + received, frameLength);
    totalSdReadMs += frameSdMs;
    received += frameLength;
  }

  bridgeReturnToDefaultAfterStream();
  String doneLine;
  if (!bridgeReadExpectedLine("OK STREAM", doneLine, 150)) {
    if (doneLine.startsWith("ERR") || doneLine == "OK BRIDGE_SLEEP") {
      Serial.printf("GETSTREAM completion error: %s\n", doneLine.c_str());
      fatalOut = true;
      return false;
    }
    Serial.printf("GETSTREAM completion not observed; validated %lu bytes by frame CRC\n",
                  (unsigned long)received);
  }
  if (!bridgeProbeDefaultControl("GETSTREAM")) {
    fatalOut = true;
    return false;
  }

  result.path = path;
  result.offset = offset;
  result.length = received;
  result.crc = combinedCrc;
  result.sdReadMs = totalSdReadMs;
  result.ok = true;
  return true;
}

bool bridgeGetChunk(const String &path, uint32_t offset, uint32_t maxBytes, ChunkResult &result) {
  result.ok = false;
  result.path = path;
  result.offset = offset;
  result.length = 0;
  result.crc = 0;
  result.sdReadMs = 0;

  if (maxBytes == 0 || maxBytes > MOTH_CHUNK_BYTES) maxBytes = MOTH_CHUNK_BYTES;

  String line;
  bool gotHeader = false;
  bool useFastPayload = bridgeFastBaudActive && !bridgeSessionBaudActive;
  const char *headerPrefix = useFastPayload ? "FASTDATA " : "DATA ";
  for (uint8_t attempt = 1; attempt <= 3; attempt++) {
    bridgeDrainInputQuiet(25, 250);
    String command = useFastPayload ? "GETFAST " : "GET ";
    bridgeSendLine(command + path + " " + String(offset) + " " + String(maxBytes));

    if (bridgeReadExpectedLineIgnoringNoise(headerPrefix, line, MOTH_DATA_HEADER_TIMEOUT_MS) && line.startsWith(headerPrefix)) {
      gotHeader = true;
      break;
    }

    Serial.printf("%s header attempt %u failed at offset %lu; last line='%s'\n",
                  useFastPayload ? "GETFAST" : "GET",
                  attempt, (unsigned long)offset, line.c_str());
    if (line == "OK BRIDGE_SLEEP") break;
    delay(100);
  }

  if (!gotHeader) {
    Serial.printf("GET expected DATA at offset %lu; got '%s'\n",
                  (unsigned long)offset, line.c_str());
    return false;
  }

  char parsedPath[128] = {0};
  unsigned long parsedOffset = 0;
  unsigned int parsedLength = 0;
  unsigned long parsedCrc = 0;
  unsigned long payloadBaud = MOTH_UART_BAUD;
  unsigned long parsedSdReadMs = 0;

  int matched = useFastPayload
      ? sscanf(line.c_str(), "FASTDATA %127s %lu %u %lx %lu %lu", parsedPath, &parsedOffset, &parsedLength, &parsedCrc, &payloadBaud, &parsedSdReadMs)
      : sscanf(line.c_str(), "DATA %127s %lu %u %lx %lu", parsedPath, &parsedOffset, &parsedLength, &parsedCrc, &parsedSdReadMs);
  int expectedFields = useFastPayload ? 5 : 4;
  if (matched < expectedFields) {
    Serial.printf("GET malformed DATA header at offset %lu: '%s'\n",
                  (unsigned long)offset, line.c_str());
    return false;
  }
  if (String(parsedPath) != path) {
    Serial.printf("GET path mismatch at offset %lu: expected '%s' got '%s'\n",
                  (unsigned long)offset, path.c_str(), parsedPath);
    return false;
  }
  if ((uint32_t)parsedOffset != offset) {
    Serial.printf("GET offset mismatch: expected %lu got %lu\n",
                  (unsigned long)offset, parsedOffset);
    return false;
  }
  if (parsedLength > MOTH_CHUNK_BYTES) {
    Serial.printf("GET length too large at offset %lu: %u\n",
                  (unsigned long)offset, parsedLength);
    return false;
  }

  if (useFastPayload && payloadBaud != MOTH_UART_FAST_BAUD) {
    Serial.printf("GETFAST baud mismatch: expected %u got %lu\n", MOTH_UART_FAST_BAUD, payloadBaud);
    return false;
  }

  if (useFastPayload) {
    MothSerial.updateBaudRate(MOTH_UART_FAST_BAUD);
    bool magicFound = bridgeReadFastMagic(MOTH_FAST_MAGIC_TIMEOUT_MS);
    bool payloadRead = magicFound && bridgeReadBytes(mothChunk, parsedLength, MOTH_BINARY_TIMEOUT_MS);
    MothSerial.updateBaudRate(MOTH_UART_BAUD);
    bridgeCurrentBaud = MOTH_UART_BAUD;
    delay(20);

    if (!magicFound) {
      Serial.printf("GETFAST preamble timeout at offset %lu\n", (unsigned long)offset);
      bridgeFlushInput();
      return false;
    }
    if (!payloadRead) {
      Serial.printf("GETFAST binary timeout at offset %lu length %u\n",
                    (unsigned long)offset, parsedLength);
      bridgeFlushInput();
      return false;
    }

    String doneLine;
    if (!bridgeReadExpectedLine("OK FASTDATA", doneLine, 1000)) {
      if (doneLine.startsWith("ERR") || doneLine == "OK BRIDGE_SLEEP") {
        Serial.printf("GETFAST completion error at offset %lu; got '%s'\n",
                      (unsigned long)offset, doneLine.c_str());
        return false;
      }
      Serial.printf("GETFAST completion not observed at offset %lu; validating payload by CRC\n",
                    (unsigned long)offset);
    }
  } else if (!bridgeReadBytes(mothChunk, parsedLength, MOTH_BINARY_TIMEOUT_MS)) {
    Serial.printf("GET binary timeout at offset %lu length %u\n",
                  (unsigned long)offset, parsedLength);
    return false;
  }

  uint32_t localCrc = crc32Update(0, mothChunk, parsedLength);
  if (localCrc != (uint32_t)parsedCrc) {
    Serial.printf("CRC mismatch: local=%08lX moth=%08lX\n", (unsigned long)localCrc, parsedCrc);
    return false;
  }

  result.path = String(parsedPath);
  result.offset = (uint32_t)parsedOffset;
  result.length = (uint32_t)parsedLength;
  result.crc = (uint32_t)parsedCrc;
  result.sdReadMs = (uint32_t)parsedSdReadMs;
  result.ok = true;
  return true;
}

bool bridgeDelete(const String &path) {
  String line;
  return bridgeExpectResponse("DELETE " + path, "OK DELETE", &line);
}

void bridgeDone() {
  bridgeSendLine("DONE");
  delay(20);
}

void bridgeRestoreDefaultBaud() {
  if (!bridgeFastBaudActive && !bridgeSessionBaudActive && bridgeCurrentBaud == MOTH_UART_BAUD) return;
  bridgeFastBaudActive = false;
  bridgeSessionBaudActive = false;
  bridgeRestartUart(MOTH_UART_BAUD, true);
}

uint32_t bridgeCurrentBaudRate() {
  return bridgeCurrentBaud;
}

uint32_t crc32Update(uint32_t crc, const uint8_t *data, uint32_t length) {
  static const uint32_t table[16] = {
    0x00000000UL, 0x1DB71064UL, 0x3B6E20C8UL, 0x26D930ACUL,
    0x76DC4190UL, 0x6B6B51F4UL, 0x4DB26158UL, 0x5005713CUL,
    0xEDB88320UL, 0xF00F9344UL, 0xD6D6A3E8UL, 0xCB61B38CUL,
    0x9B64C2B0UL, 0x86D3D2D4UL, 0xA00AE278UL, 0xBDBDF21CUL,
  };

  crc = ~crc;
  for (uint32_t i = 0; i < length; i++) {
    crc ^= data[i];
    crc = (crc >> 4) ^ table[crc & 0x0FUL];
    crc = (crc >> 4) ^ table[crc & 0x0FUL];
  }
  return ~crc;
}
