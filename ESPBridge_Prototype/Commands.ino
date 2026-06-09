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
  if (!openBridge()) {
    Serial.println("Boot probe failed.");
    return;
  }

  expectOk("PING");
  commandStatus();
  closeBridge();
  Serial.println("Boot probe complete.");
}
