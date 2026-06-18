void initMothBridge() {
  pinMode(PIN_MOTH_REQ, OUTPUT);
  digitalWrite(PIN_MOTH_REQ, MOTH_ASSERT_REQ_AT_BOOT ? HIGH : LOW);

  pinMode(PIN_MOTH_BUSY, INPUT_PULLDOWN);

  pinMode(PIN_MOTH_UART_RX, INPUT_PULLUP);
  pinMode(PIN_MOTH_UART_TX, INPUT_PULLUP);

  /* Leave the configured UART pins owned by UART2 after begin(). Calling pinMode() on
     either UART pin after this can detach the ESP32 pin matrix from Serial2. */
  MothSerial.begin(MOTH_UART_BAUD, SERIAL_8N1, PIN_MOTH_UART_RX, PIN_MOTH_UART_TX);
  while (MothSerial.available()) MothSerial.read();
}

uint32_t bridgeRawBytesRead = 0;
uint32_t bridgeLinesRead = 0;

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
  String line;
  return bridgeExpectResponse("PING", "OK PONG", &line);
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
    if (line.startsWith("ERR")) return false;
    if (bridgeIsAsyncLine(line)) continue;
    if (line == "OK BRIDGE_SLEEP") return false;

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
      }
    }
  }

  return false;
}

bool bridgeGetChunk(const String &path, uint32_t offset, uint32_t maxBytes, ChunkResult &result) {
  result.ok = false;
  result.path = path;
  result.offset = offset;
  result.length = 0;
  result.crc = 0;

  if (maxBytes == 0 || maxBytes > MOTH_CHUNK_BYTES) maxBytes = MOTH_CHUNK_BYTES;

  String line;
  bool gotHeader = false;
  for (uint8_t attempt = 1; attempt <= 3; attempt++) {
    bridgeFlushInput();
    bridgeSendLine("GET " + path + " " + String(offset) + " " + String(maxBytes));

    if (bridgeReadExpectedLine("DATA ", line, MOTH_DATA_HEADER_TIMEOUT_MS) && line.startsWith("DATA ")) {
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

  int matched = sscanf(line.c_str(), "DATA %127s %lu %u %lx", parsedPath, &parsedOffset, &parsedLength, &parsedCrc);
  if (matched != 4) {
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

uint32_t crc32Update(uint32_t crc, const uint8_t *data, uint32_t length) {
  crc = ~crc;
  for (uint32_t i = 0; i < length; i++) {
    crc ^= data[i];
    for (uint32_t j = 0; j < 8; j++) {
      uint32_t mask = -(crc & 1U);
      crc = (crc >> 1) ^ (0xEDB88320UL & mask);
    }
  }
  return ~crc;
}
