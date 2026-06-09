void initMothBridge() {
  pinMode(PIN_MOTH_REQ, OUTPUT);
  digitalWrite(PIN_MOTH_REQ, MOTH_ASSERT_REQ_AT_BOOT ? HIGH : LOW);

  pinMode(PIN_MOTH_BUSY, INPUT_PULLDOWN);

  pinMode(PIN_MOTH_UART_RX, INPUT_PULLUP);
  pinMode(PIN_MOTH_UART_TX, INPUT_PULLUP);

  /* Leave GPIO16/GPIO17 owned by UART2 after begin(). Calling pinMode() on
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

bool bridgeExpectOk(const String &cmd, String *responseOut = nullptr) {
  bridgeSendLine(cmd);
  String line;
  if (!bridgeReadLine(line, MOTH_LINE_TIMEOUT_MS)) return false;
  if (responseOut) *responseOut = line;
  return line.startsWith("OK");
}

bool bridgePing() {
  String line;
  return bridgeExpectOk("PING", &line);
}

bool bridgeSetTime(uint32_t epochUtc, uint32_t milliseconds) {
  String cmd = "TIME " + String(epochUtc) + " " + String(milliseconds);
  String line;
  return bridgeExpectOk(cmd, &line);
}

bool bridgeStatus(String &statusOut) {
  return bridgeExpectOk("STATUS", &statusOut);
}

bool bridgeList(MothFile *files, size_t maxFiles, size_t &countOut) {
  countOut = 0;
  bridgeSendLine("LIST");

  uint32_t start = millis();
  while (millis() - start < MOTH_LINE_TIMEOUT_MS) {
    String line;
    if (!bridgeReadLine(line, 1000)) continue;

    if (line == "END") return true;
    if (line.startsWith("ERR")) return false;

    if (line.startsWith("FILE ")) {
      int firstSpace = line.indexOf(' ', 5);
      if (firstSpace < 0) continue;

      String path = line.substring(5, firstSpace);
      uint32_t size = (uint32_t)strtoul(line.substring(firstSpace + 1).c_str(), nullptr, 10);

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

  bridgeSendLine("GET " + path + " " + String(offset) + " " + String(maxBytes));

  String line;
  if (!bridgeReadLine(line, MOTH_LINE_TIMEOUT_MS)) return false;
  if (line.startsWith("ERR")) return false;
  if (!line.startsWith("DATA ")) return false;

  char parsedPath[128] = {0};
  unsigned long parsedOffset = 0;
  unsigned int parsedLength = 0;
  unsigned long parsedCrc = 0;

  int matched = sscanf(line.c_str(), "DATA %127s %lu %u %lx", parsedPath, &parsedOffset, &parsedLength, &parsedCrc);
  if (matched != 4) return false;
  if ((uint32_t)parsedOffset != offset) return false;
  if (parsedLength > MOTH_CHUNK_BYTES) return false;

  if (!bridgeReadBytes(mothChunk, parsedLength, MOTH_LINE_TIMEOUT_MS)) return false;

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
  return bridgeExpectOk("DELETE " + path, &line);
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