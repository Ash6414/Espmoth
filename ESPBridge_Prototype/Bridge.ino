void initBridgePins() {
  pinMode(PIN_MOTH_REQ, OUTPUT);
  digitalWrite(PIN_MOTH_REQ, LOW);

  pinMode(PIN_MOTH_BUSY, INPUT_PULLUP);
  pinMode(PIN_MOTH_UART_RX, INPUT_PULLUP);

  MothSerial.begin(MOTH_UART_BAUD, SERIAL_8N1, PIN_MOTH_UART_RX, PIN_MOTH_UART_TX);
  pinMode(PIN_MOTH_UART_RX, INPUT_PULLUP);
  flushMothInput();
}

void resetMothParser() {
  mothLineBuffer = "";
  mothRxBytesSeen = 0;
}

uint32_t mothRxByteCount() {
  return mothRxBytesSeen;
}

uint32_t mothPartialByteCount() {
  return mothLineBuffer.length();
}

void flushMothInput() {
  while (MothSerial.available()) MothSerial.read();
  resetMothParser();
}

void setRequest(bool state) {
  digitalWrite(PIN_MOTH_REQ, state ? HIGH : LOW);
}

bool mothBusy() {
  return digitalRead(PIN_MOTH_BUSY) == HIGH;
}

void printPins() {
  Serial.printf("Pins: REQ_GPIO=%d BUSY_GPIO=%d UART_RX_GPIO=%d UART_TX_GPIO=%d baud=%lu\n",
                PIN_MOTH_REQ,
                PIN_MOTH_BUSY,
                PIN_MOTH_UART_RX,
                PIN_MOTH_UART_TX,
                (unsigned long)MOTH_UART_BAUD);
  Serial.printf("Levels: REQ=%d BUSY=%d\n",
                digitalRead(PIN_MOTH_REQ),
                digitalRead(PIN_MOTH_BUSY));
}

bool readMothLine(String &line, uint32_t timeoutMs) {
  line = "";
  uint32_t start = millis();

  while (millis() - start < timeoutMs) {
    while (MothSerial.available()) {
      char c = (char)MothSerial.read();
      mothRxBytesSeen += 1;
      if (c == '\r') continue;
      if (c == '\n') {
        if (mothLineBuffer.length() == 0) continue;
        line = mothLineBuffer;
        mothLineBuffer = "";
        return true;
      }
      if (mothLineBuffer.length() < 220) mothLineBuffer += c;
    }
    delay(1);
  }

  return false;
}

void sendMothLine(const String &line) {
  Serial.print("ESP->MOTH ");
  Serial.println(line);
  MothSerial.print(line);
  MothSerial.print('\n');
  MothSerial.flush();
}

bool waitForBusyLow(uint32_t timeoutMs) {
  uint32_t start = millis();
  while (millis() - start < timeoutMs) {
    if (!mothBusy()) return true;
    delay(25);
  }
  return !mothBusy();
}

bool waitForReadyOrPong(uint32_t timeoutMs) {
  uint32_t start = millis();
  uint32_t lastProbe = 0;
  String line;

  while (millis() - start < timeoutMs) {
    if (readMothLine(line, 250)) {
      Serial.print("MOTH->ESP ");
      Serial.println(line);

      if (line == "OK BRIDGE_READY") return true;
      if (line == "OK PONG") return true;
      if (line.startsWith("ERR")) return false;
    }

    uint32_t elapsed = millis() - start;
    if (elapsed - lastProbe >= READY_PROBE_INTERVAL_MS) {
      sendMothLine("PING");
      lastProbe = elapsed;
    }
  }

  return false;
}

bool openBridge() {
  if (bridgeOpen) {
    Serial.println("Bridge already open.");
    return true;
  }

  flushMothInput();
  setRequest(true);

  Serial.println("REQ asserted. Waiting for BUSY low...");
  if (!waitForBusyLow(MOTH_BUSY_WAIT_MS)) {
    Serial.printf("FAIL: BUSY stayed high for %lu ms. REQ=%d BUSY=%d\n",
                  (unsigned long)MOTH_BUSY_WAIT_MS,
                  digitalRead(PIN_MOTH_REQ),
                  digitalRead(PIN_MOTH_BUSY));
    setRequest(false);
    return false;
  }

  Serial.println("BUSY is low. Waiting for READY/PONG...");
  if (!waitForReadyOrPong(MOTH_READY_WAIT_MS)) {
    Serial.printf("FAIL: no READY/PONG. REQ=%d BUSY=%d\n",
                  digitalRead(PIN_MOTH_REQ),
                  digitalRead(PIN_MOTH_BUSY));
    Serial.println("Check AudioMoth switch is CUSTOM and firmware includes ESPBridge_serviceUntil().");
    setRequest(false);
    return false;
  }

  bridgeOpen = true;
  Serial.println("Bridge open.");
  return true;
}

void closeBridge() {
  if (bridgeOpen) {
    sendMothLine("DONE");
    String line;
    if (readMothLine(line, MOTH_LINE_WAIT_MS)) {
      Serial.print("MOTH->ESP ");
      Serial.println(line);
    }
  }

  bridgeOpen = false;
  setRequest(false);
  flushMothInput();
  Serial.println("Bridge closed.");
}

bool expectOk(const String &command, String *responseOut) {
  if (!bridgeOpen && !openBridge()) return false;

  sendMothLine(command);

  String line;
  if (!readMothLine(line, MOTH_LINE_WAIT_MS)) {
    Serial.println("FAIL: no response.");
    return false;
  }

  Serial.print("MOTH->ESP ");
  Serial.println(line);
  if (responseOut) *responseOut = line;
  return line.startsWith("OK");
}
