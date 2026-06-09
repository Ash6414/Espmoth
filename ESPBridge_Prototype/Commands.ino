void printHelp() {
  Serial.println();
  Serial.println("Commands:");
  Serial.println("  help              Show this help");
  Serial.println("  pins              Print REQ/BUSY/UART pins");
  Serial.println("  open              Assert REQ and open bridge");
  Serial.println("  ping              Send PING");
  Serial.println("  status            Send STATUS");
  Serial.println("  list              Send LIST and print FILE lines");
  Serial.println("  time <epoch>      Send TIME <epoch> 0");
  Serial.println("  raw <command>     Send a raw bridge command");
  Serial.println("  reqprobe <sec>    Hold REQ high, send PINGs, log BUSY/UART");
  Serial.println("  rxdiag <sec>      Capture GPIO RX edge timing and try baud decodes");
  Serial.println("  swapprobe <sec>   Run reqprobe with ESP RX/TX pins swapped");
  Serial.println("  watch <sec>       Log REQ/BUSY without changing pins");
  Serial.println("  done              Send DONE and deassert REQ");
  Serial.println("  swap 0|1          Reconfigure ESP UART pins normal/swapped");
  Serial.println("  req 0|1           Manually set ESP_REQ");
  Serial.println("  flush             Clear ESP UART RX buffer");
  Serial.println();
}

void commandStatus() {
  String response;
  expectOk("STATUS", &response);
}

void commandList() {
  if (!bridgeOpen && !openBridge()) return;

  sendMothLine("LIST");

  uint32_t start = millis();
  uint32_t files = 0;
  String line;

  while (millis() - start < MOTH_LINE_WAIT_MS) {
    if (!readMothLine(line, 500)) continue;

    Serial.print("MOTH->ESP ");
    Serial.println(line);

    if (line == "END") {
      Serial.printf("LIST complete. Files: %lu\n", (unsigned long)files);
      return;
    }
    if (line.startsWith("ERR")) {
      Serial.println("LIST failed.");
      return;
    }
    if (line.startsWith("FILE ")) files += 1;
  }

  Serial.println("LIST timed out.");
}

void commandSetTime(const String &arg) {
  String epoch = arg;
  epoch.trim();
  if (epoch.length() == 0) {
    Serial.println("Usage: time <unix_epoch>");
    return;
  }
  expectOk("TIME " + epoch + " 0");
}

uint32_t parseSecondsOrDefault(const String &arg, uint32_t defaultSeconds) {
  String value = arg;
  value.trim();
  if (value.length() == 0) return defaultSeconds;

  uint32_t seconds = (uint32_t)value.toInt();
  if (seconds == 0) return defaultSeconds;
  if (seconds > 120) return 120;
  return seconds;
}

void printProbeSample(uint32_t elapsedMs, int &lastReq, int &lastBusy, int &lastRx, bool force) {
  int req = digitalRead(PIN_MOTH_REQ);
  int busy = digitalRead(PIN_MOTH_BUSY);
  int rx = digitalRead(mothUartRxPin);
  if (!force && req == lastReq && busy == lastBusy && rx == lastRx) return;

  Serial.printf("%lu ms: REQ=%d BUSY=%d UART_RX_LEVEL=%d UART_AVAIL=%d UART_BYTES=%lu PARTIAL=%lu\n",
                (unsigned long)elapsedMs,
                req,
                busy,
                rx,
                MothSerial.available(),
                (unsigned long)mothRxByteCount(),
                (unsigned long)mothPartialByteCount());
  lastReq = req;
  lastBusy = busy;
  lastRx = rx;
}

void commandWatchPins(uint32_t seconds) {
  Serial.printf("Watching pins for %lu second(s).\n", (unsigned long)seconds);

  uint32_t durationMs = seconds * 1000UL;
  uint32_t start = millis();
  uint32_t lastPrintMs = 0;
  int lastReq = -1;
  int lastBusy = -1;
  int lastRx = -1;
  ProbeEdges edges;

  while (millis() - start < durationMs) {
    uint32_t elapsed = millis() - start;
    updateProbeEdges(edges, elapsed);
    bool force = elapsed - lastPrintMs >= 1000;
    printProbeSample(elapsed, lastReq, lastBusy, lastRx, force);
    if (force) lastPrintMs = elapsed;

    String line;
    if (readMothLine(line, 10)) {
      Serial.print("MOTH->ESP ");
      Serial.println(line);
    }
  }

  printProbeSample(millis() - start, lastReq, lastBusy, lastRx, true);
  Serial.printf("Edge summary: BUSY rising=%lu falling=%lu UART_RX rising=%lu falling=%lu\n",
                (unsigned long)edges.busyRising,
                (unsigned long)edges.busyFalling,
                (unsigned long)edges.rxRising,
                (unsigned long)edges.rxFalling);
  Serial.println("Watch complete.");
}

void commandReqProbe(uint32_t seconds) {
  Serial.printf("REQ probe for %lu second(s).\n", (unsigned long)seconds);
  Serial.println("Holding ESP_REQ high, probing UART with PING.");

  flushMothInput();
  startRawRxCapture();
  setRequest(true);

  uint32_t durationMs = seconds * 1000UL;
  uint32_t start = millis();
  uint32_t lastPingMs = 0;
  uint32_t lastPrintMs = 0;
  int lastReq = -1;
  int lastBusy = -1;
  int lastRx = -1;
  bool sawBridge = false;
  bool sawBusyLow = !mothBusy();
  bool sawAnyLine = false;
  bool sawReady = false;
  bool sawPong = false;
  uint32_t lineCount = 0;
  uint32_t pingCount = 0;
  ProbeEdges edges;

  while (millis() - start < durationMs) {
    uint32_t elapsed = millis() - start;
    updateProbeEdges(edges, elapsed);
    bool force = elapsed - lastPrintMs >= 1000;
    printProbeSample(elapsed, lastReq, lastBusy, lastRx, force);
    if (force) lastPrintMs = elapsed;
    if (!mothBusy()) sawBusyLow = true;

    if (elapsed - lastPingMs >= READY_PROBE_INTERVAL_MS) {
      sendMothLine("PING");
      lastPingMs = elapsed;
      pingCount += 1;
    }

    String line;
    if (readMothLine(line, 25)) {
      sawAnyLine = true;
      lineCount += 1;
      Serial.print("MOTH->ESP ");
      Serial.println(line);
      if (line == "OK BRIDGE_READY") {
        sawBridge = true;
        sawReady = true;
      }
      if (line == "OK PONG") {
        sawBridge = true;
        sawPong = true;
      }
    }

    updateProbeEdges(edges, millis() - start);
  }

  if (sawBridge) {
    sendMothLine("DONE");
    String line;
    if (readMothLine(line, MOTH_LINE_WAIT_MS)) {
      Serial.print("MOTH->ESP ");
      Serial.println(line);
    }
  }

  setRequest(false);
  delay(25);
  stopRawRxCapture();
  printProbeSample(millis() - start, lastReq, lastBusy, lastRx, true);
  uint32_t rawBytes = mothRxByteCount();
  uint32_t partialBytes = mothPartialByteCount();
  bool rawDecodeSawBridge = printRawRxCaptureSummary();
  Serial.println();
  Serial.println("REQ probe summary:");
  Serial.printf("  PINGs sent: %lu\n", (unsigned long)pingCount);
  Serial.printf("  Raw UART bytes received: %lu\n", (unsigned long)rawBytes);
  Serial.printf("  UART lines received: %lu\n", (unsigned long)lineCount);
  Serial.printf("  Partial UART bytes buffered: %lu\n", (unsigned long)partialBytes);
  Serial.printf("  BUSY low observed: %s\n", sawBusyLow ? "YES" : "NO");
  Serial.printf("  BUSY edges: rising=%lu falling=%lu\n",
                (unsigned long)edges.busyRising,
                (unsigned long)edges.busyFalling);
  Serial.printf("  UART_RX edges: rising=%lu falling=%lu\n",
                (unsigned long)edges.rxRising,
                (unsigned long)edges.rxFalling);
  Serial.printf("  READY observed: %s\n", sawReady ? "YES" : "NO");
  Serial.printf("  PONG observed: %s\n", sawPong ? "YES" : "NO");
  Serial.printf("  Raw GPIO decode observed bridge text: %s\n", rawDecodeSawBridge ? "YES" : "NO");

  if (sawBridge) {
    Serial.println("RESULT: PASS basic ESP32 <-> AudioMoth bridge communication detected.");
  } else if (!sawBusyLow) {
    Serial.println("RESULT: FAIL MOTH_BUSY never went low while ESP_REQ was high.");
  } else if (!sawAnyLine) {
    if (rawDecodeSawBridge) {
      Serial.println("RESULT: FAIL AudioMoth TX is GPIO-decodable, but ESP32 hardware UART decoded zero bridge lines.");
    } else if (rawBytes > 0) {
      Serial.println("RESULT: FAIL UART bytes arrived, but no newline-terminated AudioMoth bridge line was received.");
    } else if (edges.rxRising > 0 || edges.rxFalling > 0) {
      Serial.println("RESULT: FAIL B9 GPIO pulse reached ESP RX, but no UART bytes decoded.");
    } else if (edges.busyRising > 0 || edges.busyFalling > 0) {
      Serial.println("RESULT: FAIL AudioMoth entered bridge service, but B9/UART did not reach ESP RX.");
    } else {
      Serial.println("RESULT: FAIL MOTH_BUSY went low, but no AudioMoth UART response was received.");
    }
  } else {
    Serial.println("RESULT: FAIL UART responded, but no OK BRIDGE_READY or OK PONG was observed.");
  }

  flushMothInput();
  Serial.println("REQ probe complete.");
}

void commandSwapProbe(uint32_t seconds) {
  bool originalSwapped = mothUartSwapped;

  Serial.println("Switching ESP UART pins to SWAPPED mode for probe.");
  configureMothUart(true);
  printPins();
  commandReqProbe(seconds);

  Serial.println("Restoring previous UART pin mode.");
  configureMothUart(originalSwapped);
  printPins();
}

void handleCommand(String command) {
  command.trim();
  String lower = command;
  lower.toLowerCase();

  if (lower == "help") {
    printHelp();
  } else if (lower == "pins") {
    printPins();
  } else if (lower == "open") {
    openBridge();
  } else if (lower == "ping") {
    expectOk("PING");
  } else if (lower == "status") {
    commandStatus();
  } else if (lower == "list") {
    commandList();
  } else if (lower.startsWith("time ")) {
    commandSetTime(command.substring(5));
  } else if (lower.startsWith("raw ")) {
    if (!bridgeOpen && !openBridge()) return;
    sendMothLine(command.substring(4));
  } else if (lower.startsWith("reqprobe")) {
    commandReqProbe(parseSecondsOrDefault(command.substring(8), 20));
  } else if (lower.startsWith("rxdiag")) {
    commandRxDiag(parseSecondsOrDefault(command.substring(6), 10));
  } else if (lower.startsWith("swapprobe")) {
    commandSwapProbe(parseSecondsOrDefault(command.substring(9), 20));
  } else if (lower.startsWith("watch")) {
    commandWatchPins(parseSecondsOrDefault(command.substring(5), 20));
  } else if (lower == "done") {
    closeBridge();
  } else if (lower == "flush") {
    flushMothInput();
    Serial.println("Flushed AudioMoth UART input.");
  } else if (lower == "swap 1") {
    configureMothUart(true);
    printPins();
  } else if (lower == "swap 0") {
    configureMothUart(false);
    printPins();
  } else if (lower == "req 1") {
    setRequest(true);
    printPins();
  } else if (lower == "req 0") {
    setRequest(false);
    printPins();
  } else {
    Serial.println("Unknown command. Type: help");
  }
}

void runBootProbe() {
  Serial.println();
  Serial.println("Boot probe starting.");
  commandReqProbe(AUTO_REQ_PROBE_SECONDS);
  Serial.println("Boot probe complete.");
}
