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
  Serial.println("  watch <sec>       Log REQ/BUSY without changing pins");
  Serial.println("  done              Send DONE and deassert REQ");
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

void printProbeSample(uint32_t elapsedMs, int &lastReq, int &lastBusy, bool force) {
  int req = digitalRead(PIN_MOTH_REQ);
  int busy = digitalRead(PIN_MOTH_BUSY);
  if (!force && req == lastReq && busy == lastBusy) return;

  Serial.printf("%lu ms: REQ=%d BUSY=%d UART_AVAIL=%d\n",
                (unsigned long)elapsedMs,
                req,
                busy,
                MothSerial.available());
  lastReq = req;
  lastBusy = busy;
}

void commandWatchPins(uint32_t seconds) {
  Serial.printf("Watching pins for %lu second(s).\n", (unsigned long)seconds);

  uint32_t durationMs = seconds * 1000UL;
  uint32_t start = millis();
  uint32_t lastPrintMs = 0;
  int lastReq = -1;
  int lastBusy = -1;

  while (millis() - start < durationMs) {
    uint32_t elapsed = millis() - start;
    bool force = elapsed - lastPrintMs >= 1000;
    printProbeSample(elapsed, lastReq, lastBusy, force);
    if (force) lastPrintMs = elapsed;

    String line;
    if (readMothLine(line, 10)) {
      Serial.print("MOTH->ESP ");
      Serial.println(line);
    }
  }

  printProbeSample(millis() - start, lastReq, lastBusy, true);
  Serial.println("Watch complete.");
}

void commandReqProbe(uint32_t seconds) {
  Serial.printf("REQ probe for %lu second(s).\n", (unsigned long)seconds);
  Serial.println("Holding ESP_REQ high, probing UART with PING.");

  flushMothInput();
  setRequest(true);

  uint32_t durationMs = seconds * 1000UL;
  uint32_t start = millis();
  uint32_t lastPingMs = 0;
  uint32_t lastPrintMs = 0;
  int lastReq = -1;
  int lastBusy = -1;
  bool sawBridge = false;

  while (millis() - start < durationMs) {
    uint32_t elapsed = millis() - start;
    bool force = elapsed - lastPrintMs >= 1000;
    printProbeSample(elapsed, lastReq, lastBusy, force);
    if (force) lastPrintMs = elapsed;

    if (elapsed - lastPingMs >= READY_PROBE_INTERVAL_MS) {
      sendMothLine("PING");
      lastPingMs = elapsed;
    }

    String line;
    if (readMothLine(line, 25)) {
      Serial.print("MOTH->ESP ");
      Serial.println(line);
      if (line == "OK BRIDGE_READY" || line == "OK PONG") sawBridge = true;
    }
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
  printProbeSample(millis() - start, lastReq, lastBusy, true);
  flushMothInput();
  Serial.println("REQ probe complete.");
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
  } else if (lower.startsWith("watch")) {
    commandWatchPins(parseSecondsOrDefault(command.substring(5), 20));
  } else if (lower == "done") {
    closeBridge();
  } else if (lower == "flush") {
    flushMothInput();
    Serial.println("Flushed AudioMoth UART input.");
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
