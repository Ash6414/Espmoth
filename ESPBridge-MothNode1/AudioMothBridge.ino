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
uint32_t bridgeCurrentBaud = MOTH_UART_BAUD;
bool bridgePipeStreamSupported = true;

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
      if (line.length() < 320) line += c;
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

bool bridgeIsAsyncLine(const String &line) {
  return line == "OK BRIDGE_READY" || line == "OK PONG";
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
  if (!bridgeReadExpectedLine(expectedPrefix, line, MOTH_LINE_TIMEOUT_MS)) {
    if (responseOut) *responseOut = line;
    return false;
  }
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

bool bridgeSetTime(uint32_t epochUtc, uint32_t milliseconds) {
  String cmd = "TIME " + String(epochUtc) + " " + String(milliseconds);
  String line;
  return bridgeExpectResponse(cmd, "OK TIME", &line);
}

bool bridgeStatus(String &statusOut) {
  return bridgeExpectResponse("STATUS", "OK STATUS", &statusOut);
}

bool bridgeStatusFieldUInt(const String &status, const char *key, unsigned long &valueOut) {
  String needle = String(key) + "=";
  int start = status.indexOf(needle);
  if (start < 0) return false;
  start += needle.length();

  int end = start;
  while (end < (int)status.length() && status[end] >= '0' && status[end] <= '9') {
    end += 1;
  }
  if (end == start) return false;

  valueOut = strtoul(status.substring(start, end).c_str(), nullptr, 10);
  return true;
}

void bridgeApplyStatusCapabilities(const String &status) {
  unsigned long protocol = 0;
  bool hasProtocol = bridgeStatusFieldUInt(status, "proto", protocol);

#if MOTH_PIPE_FAST_ENABLED
  unsigned long pipe = 0;
  unsigned long pipeBaud = 0;
  unsigned long pipeBytes = 0;
  unsigned long pipeFrame = 0;
  unsigned long pipeAck = 0;
  bool hasPipe = bridgeStatusFieldUInt(status, "pipe", pipe);
  bool hasPipeBaud = bridgeStatusFieldUInt(status, "pipe_baud", pipeBaud);
  bool hasPipeBytes = bridgeStatusFieldUInt(status, "pipe_bytes", pipeBytes);
  bool hasPipeFrame = bridgeStatusFieldUInt(status, "pipe_frame", pipeFrame);
  bool hasPipeAck = bridgeStatusFieldUInt(status, "pipe_ack", pipeAck);

  if (!hasProtocol || protocol < 4 || !hasPipe || !hasPipeBaud || !hasPipeBytes || !hasPipeFrame || !hasPipeAck) {
    if (bridgePipeStreamSupported) {
      Serial.println("AudioMoth STATUS lacks protocol v4 ACKed pipe fields; high-speed upload requires the current v4 AudioMoth bin");
    }
    bridgePipeStreamSupported = false;
  } else if (pipe != 1 || pipeBaud != MOTH_PIPE_FAST_BAUD || pipeBytes < SERVER_UPLOAD_CHUNK_BYTES ||
             pipeFrame == 0 || pipeFrame > MOTH_CHUNK_BYTES || pipeAck != 1) {
    Serial.printf("AudioMoth pipe capability mismatch: proto=%lu pipe=%lu pipe_baud=%lu pipe_bytes=%lu pipe_frame=%lu pipe_ack=%lu expected_baud=%u expected_bytes=%u expected_frame<=%u\n",
                  protocol, pipe, pipeBaud, pipeBytes, pipeFrame, pipeAck,
                  MOTH_PIPE_FAST_BAUD, SERVER_UPLOAD_CHUNK_BYTES, MOTH_CHUNK_BYTES);
    bridgePipeStreamSupported = false;
  } else {
    bridgePipeStreamSupported = true;
    Serial.printf("AudioMoth pipe capability OK: proto=%lu baud=%lu bytes=%lu frame=%lu ack=%lu\n",
                  protocol, pipeBaud, pipeBytes, pipeFrame, pipeAck);
  }
#endif
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

bool bridgeRunTestStream(uint32_t requestedBytes, uint32_t baud, uint32_t &receivedOut, uint32_t &elapsedMsOut, uint32_t &crcOut) {
  receivedOut = 0;
  elapsedMsOut = 0;
  crcOut = 0;

  if (requestedBytes == 0) requestedBytes = MOTH_TEST_STREAM_BYTES;
  if (requestedBytes > MOTH_TEST_STREAM_BYTES) requestedBytes = MOTH_TEST_STREAM_BYTES;
  if (baud == 0) baud = MOTH_STREAM_TEST_BAUD_3;
  if (baud == MOTH_UART_BAUD) return false;
  if (bridgeCurrentBaud != MOTH_UART_BAUD) {
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

bool bridgeStartPipeStream(const String &path, uint32_t offset, uint32_t requestedBytes, PipeSession &pipe, bool &unsupportedOut) {
  pipe.active = false;
  pipe.path = path;
  pipe.startOffset = offset;
  pipe.totalBytes = 0;
  pipe.receivedBytes = 0;
  pipe.blockMaxBytes = 0;
  pipe.frameMaxBytes = 0;
  pipe.baud = 0;
  unsupportedOut = false;

#if !MOTH_PIPE_FAST_ENABLED
  return false;
#endif

  if (!bridgePipeStreamSupported) {
    unsupportedOut = true;
    return false;
  }
  if (requestedBytes == 0) {
    return false;
  }
  if (bridgeCurrentBaud != MOTH_UART_BAUD) {
    return false;
  }

  bridgeDrainInputQuiet(25, 250);
  bridgeSendLine("GETPIPE " + path + " " + String(offset) + " " + String(requestedBytes) + " " + String(MOTH_PIPE_FAST_BAUD));

  String line;
  if (!bridgeReadExpectedLineIgnoringNoise("PIPE ", line, MOTH_DATA_HEADER_TIMEOUT_MS) || !line.startsWith("PIPE ")) {
    if (line.startsWith("ERR CMD") || line.startsWith("ERR ARG")) {
      Serial.printf("AudioMoth GETPIPE is unavailable: %s\n", line.c_str());
      bridgePipeStreamSupported = false;
      unsupportedOut = true;
    } else if (line.length()) {
      Serial.printf("GETPIPE header failed at offset %lu: %s\n", (unsigned long)offset, line.c_str());
    } else {
      Serial.printf("GETPIPE header timed out at offset %lu\n", (unsigned long)offset);
    }
    bridgeFlushInput();
    return false;
  }

  char parsedPath[128] = {0};
  unsigned long parsedOffset = 0;
  unsigned long totalBytes = 0;
  unsigned long blockMax = 0;
  unsigned int frameMax = 0;
  unsigned long pipeBaud = 0;
  if (sscanf(line.c_str(), "PIPE %127s %lu %lu %lu %u %lu",
             parsedPath, &parsedOffset, &totalBytes, &blockMax, &frameMax, &pipeBaud) != 6) {
    Serial.printf("GETPIPE malformed header: %s\n", line.c_str());
    return false;
  }
  if (String(parsedPath) != path || parsedOffset != offset || totalBytes == 0 ||
      totalBytes > requestedBytes || blockMax == 0 ||
      frameMax == 0 || frameMax > MOTH_CHUNK_BYTES || pipeBaud != MOTH_PIPE_FAST_BAUD) {
    Serial.printf("GETPIPE header mismatch: %s\n", line.c_str());
    return false;
  }

  pipe.active = true;
  pipe.path = path;
  pipe.startOffset = (uint32_t)parsedOffset;
  pipe.totalBytes = (uint32_t)totalBytes;
  pipe.receivedBytes = 0;
  pipe.blockMaxBytes = (uint32_t)blockMax;
  pipe.frameMaxBytes = (uint32_t)frameMax;
  pipe.baud = (uint32_t)pipeBaud;
  Serial.printf("GETPIPE started for %s at %lu: total=%lu block=%lu frame=%lu baud=%lu\n",
                path.c_str(),
                (unsigned long)pipe.startOffset,
                (unsigned long)pipe.totalBytes,
                (unsigned long)pipe.blockMaxBytes,
                (unsigned long)pipe.frameMaxBytes,
                (unsigned long)pipe.baud);
  return true;
}

bool bridgeReadPipeBlock(PipeSession &pipe, uint8_t *dest, ChunkResult &result, bool &doneOut, bool &fatalOut) {
  result.ok = false;
  result.path = pipe.path;
  result.offset = pipe.startOffset + pipe.receivedBytes;
  result.length = 0;
  result.crc = 0;
  result.sdReadMs = 0;
  doneOut = false;
  fatalOut = false;

#if !MOTH_PIPE_FAST_ENABLED
  return false;
#endif

  if (!pipe.active || !dest || pipe.receivedBytes >= pipe.totalBytes ||
      pipe.blockMaxBytes == 0 ||
      pipe.frameMaxBytes == 0 || pipe.frameMaxBytes > MOTH_CHUNK_BYTES ||
      pipe.baud != MOTH_PIPE_FAST_BAUD) {
    return false;
  }

  uint32_t blockOffset = pipe.startOffset + pipe.receivedBytes;
  uint32_t remaining = pipe.totalBytes - pipe.receivedBytes;
  uint32_t blockTarget = remaining > pipe.blockMaxBytes ? pipe.blockMaxBytes : remaining;
  if (blockTarget > SERVER_UPLOAD_CHUNK_BYTES) {
    Serial.printf("GETPIPE block target %lu exceeds ESP upload buffer %u\n",
                  (unsigned long)blockTarget, SERVER_UPLOAD_CHUNK_BYTES);
    return false;
  }

  MothSerial.updateBaudRate(pipe.baud);
  bridgeCurrentBaud = pipe.baud;

  static const uint8_t streamMagic[] = {0xA5, 0x5A, 0xD7, 0x7D};
  uint8_t frameHeader[14];
  uint32_t received = 0;
  uint32_t combinedCrc = 0;
  uint32_t totalSdReadMs = 0;

  while (received < blockTarget) {
    uint32_t expectedOffset = blockOffset + received;
    uint16_t acceptedLength = 0;
    uint32_t acceptedSdMs = 0;
    bool accepted = false;

    for (uint8_t attempt = 0; attempt <= MOTH_PIPE_FRAME_RETRIES && !accepted; attempt += 1) {
      if (!bridgeReadMagic(streamMagic, sizeof(streamMagic), MOTH_STREAM_FRAME_TIMEOUT_MS)) {
        Serial.printf("GETPIPE frame magic timeout at offset %lu attempt %u\n",
                      (unsigned long)expectedOffset, attempt + 1);
        bridgeSendLine("NAK " + String(expectedOffset) + " 0");
        continue;
      }

      if (!bridgeReadBytes(frameHeader, sizeof(frameHeader), MOTH_STREAM_FRAME_TIMEOUT_MS)) {
        Serial.printf("GETPIPE frame header timeout at offset %lu attempt %u\n",
                      (unsigned long)expectedOffset, attempt + 1);
        bridgeSendLine("NAK " + String(expectedOffset) + " 0");
        continue;
      }

      uint32_t frameOffset = bridgeReadUInt32LE(frameHeader);
      uint16_t frameLength = bridgeReadUInt16LE(frameHeader + 4);
      uint32_t frameCrc = bridgeReadUInt32LE(frameHeader + 6);
      uint32_t frameSdMs = bridgeReadUInt32LE(frameHeader + 10);

      if (frameOffset != expectedOffset || frameLength == 0 || frameLength > pipe.frameMaxBytes || received + frameLength > blockTarget) {
        Serial.printf("GETPIPE frame metadata mismatch: frame_offset=%lu expected=%lu len=%u attempt=%u\n",
                      (unsigned long)frameOffset, (unsigned long)expectedOffset, frameLength, attempt + 1);
        bridgeSendLine("NAK " + String(expectedOffset) + " 0");
        continue;
      }

      if (!bridgeReadBytes(dest + received, frameLength, MOTH_BINARY_TIMEOUT_MS)) {
        Serial.printf("GETPIPE payload timeout at offset %lu length %u attempt %u\n",
                      (unsigned long)frameOffset, frameLength, attempt + 1);
        bridgeSendLine("NAK " + String(expectedOffset) + " " + String(frameLength));
        continue;
      }

      uint32_t localCrc = crc32Update(0, dest + received, frameLength);
      if (localCrc != frameCrc) {
        Serial.printf("GETPIPE CRC mismatch at offset %lu attempt %u: local=%08lX moth=%08lX\n",
                      (unsigned long)frameOffset, attempt + 1, (unsigned long)localCrc, (unsigned long)frameCrc);
        bridgeSendLine("NAK " + String(expectedOffset) + " " + String(frameLength));
        continue;
      }

      bridgeSendLine("ACK " + String(frameOffset) + " " + String(frameLength));
      acceptedLength = frameLength;
      acceptedSdMs = frameSdMs;
      accepted = true;
    }

    if (!accepted) {
      Serial.printf("GETPIPE frame failed after retries at offset %lu\n", (unsigned long)expectedOffset);
      fatalOut = true;
      bridgeRestartUart(MOTH_UART_BAUD, true);
      return false;
    }

    combinedCrc = crc32Update(combinedCrc, dest + received, acceptedLength);
    totalSdReadMs += acceptedSdMs;
    received += acceptedLength;
  }

  bridgeReturnToDefaultAfterStream();

  String blockLine;
  if (!bridgeReadExpectedLine("OK PIPEBLOCK", blockLine, 2500)) {
    if (blockLine.startsWith("ERR") || blockLine == "OK BRIDGE_SLEEP") {
      Serial.printf("GETPIPE block completion error: %s\n", blockLine.c_str());
      fatalOut = true;
      return false;
    }
    Serial.printf("GETPIPE block completion not observed at offset %lu\n", (unsigned long)blockOffset);
    fatalOut = true;
    return false;
  }

  char parsedPath[128] = {0};
  unsigned long parsedOffset = 0;
  unsigned long parsedLength = 0;
  if (sscanf(blockLine.c_str(), "OK PIPEBLOCK %127s %lu %lu", parsedPath, &parsedOffset, &parsedLength) != 3 ||
      String(parsedPath) != pipe.path || parsedOffset != blockOffset || parsedLength != received) {
    Serial.printf("GETPIPE block completion mismatch: %s\n", blockLine.c_str());
    fatalOut = true;
    return false;
  }

  pipe.receivedBytes += received;
  result.path = pipe.path;
  result.offset = blockOffset;
  result.length = received;
  result.crc = combinedCrc;
  result.sdReadMs = totalSdReadMs;
  result.ok = true;

  if (pipe.receivedBytes >= pipe.totalBytes) {
    String doneLine;
    if (!bridgeReadExpectedLine("OK PIPEDONE", doneLine, 2500)) {
      if (doneLine.startsWith("ERR") || doneLine == "OK BRIDGE_SLEEP") {
        Serial.printf("GETPIPE done error: %s\n", doneLine.c_str());
        fatalOut = true;
        return false;
      }
      Serial.printf("GETPIPE done line not observed; validated %lu bytes by frame CRC\n",
                    (unsigned long)pipe.receivedBytes);
    }
    pipe.active = false;
    doneOut = true;
  }

  return true;
}

bool bridgeContinuePipeStream(const PipeSession &pipe) {
#if !MOTH_PIPE_FAST_ENABLED
  return false;
#endif
  if (!pipe.active || pipe.receivedBytes >= pipe.totalBytes) return false;
  uint32_t nextOffset = pipe.startOffset + pipe.receivedBytes;
  bridgeSendLine("NEXT " + String(nextOffset));
  return true;
}

void bridgeStopPipeStream() {
#if MOTH_PIPE_FAST_ENABLED
  if (bridgeCurrentBaud != MOTH_UART_BAUD) {
    bridgeRestartUart(MOTH_UART_BAUD, true);
  }
  bridgeSendLine("STOP");
  String line;
  bridgeReadExpectedLine("OK PIPESTOP", line, 500);
#endif
}

#if MOTH_ALLOW_115200_GET_FALLBACK
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
  for (uint8_t attempt = 1; attempt <= 3; attempt++) {
    bridgeDrainInputQuiet(25, 250);
    bridgeSendLine("GET " + path + " " + String(offset) + " " + String(maxBytes));

    if (bridgeReadExpectedLineIgnoringNoise("DATA ", line, MOTH_DATA_HEADER_TIMEOUT_MS) && line.startsWith("DATA ")) {
      gotHeader = true;
      break;
    }

    Serial.printf("GET header attempt %u failed at offset %lu; last line='%s'\n",
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
  unsigned long parsedSdReadMs = 0;

  int matched = sscanf(line.c_str(), "DATA %127s %lu %u %lx %lu",
                       parsedPath, &parsedOffset, &parsedLength, &parsedCrc, &parsedSdReadMs);
  if (matched < 4) {
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

  if (!bridgeReadBytes(mothChunk, parsedLength, MOTH_BINARY_TIMEOUT_MS)) {
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
#endif

bool bridgeDelete(const String &path) {
  String line;
  return bridgeExpectResponse("DELETE " + path, "OK DELETE", &line);
}

void bridgeDone() {
  bridgeSendLine("DONE");
  delay(20);
}

void bridgeRestoreDefaultBaud() {
  if (bridgeCurrentBaud == MOTH_UART_BAUD) return;
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
