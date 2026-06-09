/*
  AudioMoth ESPBridge Diagnostic Sketch - Option A

  Purpose:
  - Verifies the ESP32 <-> AudioMoth bridge electrically and logically without the server.
  - Option A only: dedicated ESPBridge. GPS spoofing/PPS/NMEA are disabled.
  - Uses a7 as ESP_REQ and a8 as MOTH_BUSY. Do not run this against GPS-spoof firmware.

  Wiring:
    ESP32 GPIO16 RX2  <- AudioMoth b9 UART TX
    ESP32 GPIO17 TX2  -> AudioMoth b10 UART RX
    ESP32 GPIO25 OUT  -> AudioMoth a7 ESP_REQ
    ESP32 GPIO26 IN   <- AudioMoth a8 MOTH_BUSY
    GND common

  AudioMoth bridge UART: 921600 baud, 8N1

  Serial Monitor:
    115200 baud

  Commands:
    open
    ping
    status
    time 1781015124
    list
    done
    test 1781015124

  Recommended first test:
    test <current_server_epoch>
*/

#include <Arduino.h>

// ----------------------------
// ESP32 <-> AudioMoth bridge pins
// ----------------------------
static constexpr int PIN_MOTH_UART_RX = 16;  // ESP32 RX2  <- AudioMoth b9 TX
static constexpr int PIN_MOTH_UART_TX = 17;  // ESP32 TX2  -> AudioMoth b10 RX
static constexpr int PIN_MOTH_REQ     = 25;  // ESP32 OUT  -> AudioMoth a7 ESP_REQ
static constexpr int PIN_MOTH_BUSY    = 26;  // ESP32 IN   <- AudioMoth a8 MOTH_BUSY

static constexpr uint32_t SERIAL_BAUD = 115200;
static constexpr uint32_t MOTH_UART_BAUD = 921600;
static constexpr uint32_t MOTH_BUSY_WAIT_MS = 65000;
static constexpr uint32_t MOTH_READY_TIMEOUT_MS = 20000;
static constexpr uint32_t MOTH_LINE_TIMEOUT_MS = 5000;

HardwareSerial MothSerial(2);

static bool bridgeOpen = false;

bool mothBusy() {
  return digitalRead(PIN_MOTH_BUSY) == HIGH;
}

void mothRequest(bool state) {
  digitalWrite(PIN_MOTH_REQ, state ? HIGH : LOW);
}

void bridgeFlushInput() {
  uint32_t lastByteMs = millis();
  while (millis() - lastByteMs < 25) {
    while (MothSerial.available()) {
      (void)MothSerial.read();
      lastByteMs = millis();
    }
    delay(1);
  }
}

bool isBridgePrintable(char c) {
  return c >= 32 && c <= 126;
}

void bridgeSendLine(const String &line) {
  Serial.print("MOTH << ");
  Serial.println(line);
  MothSerial.print(line);
  MothSerial.print('\n');
  MothSerial.flush();
}

bool bridgeReadLine(String &line, uint32_t timeoutMs) {
  line = "";
  uint32_t start = millis();

  while (millis() - start < timeoutMs) {
    while (MothSerial.available()) {
      char c = static_cast<char>(MothSerial.read());

      if (c == '\r') continue;

      if (c == '\n') {
        if (line.length() == 0) continue;
        Serial.print("MOTH >> ");
        Serial.println(line);
        return true;
      }

      // Bridge command responses are ASCII. Binary bytes can appear only after
      // a DATA header. The diagnostic does not request DATA payloads, so discard
      // non-ASCII residue instead of accepting garbage as a response line.
      if (!isBridgePrintable(c)) {
        Serial.printf("MOTH >> [discarded non-ASCII byte 0x%02X]\n", static_cast<uint8_t>(c));
        line = "";
        continue;
      }

      if (line.length() < 220) line += c;
    }

    delay(1);
  }

  return false;
}

bool waitForMothIdle(uint32_t waitMs) {
  uint32_t start = millis();
  while (millis() - start < waitMs) {
    if (!mothBusy()) return true;
    delay(50);
  }
  return !mothBusy();
}

bool bridgeWaitReady(uint32_t timeoutMs) {
  String line;
  uint32_t start = millis();

  while (millis() - start < timeoutMs) {
    if (bridgeReadLine(line, 500)) {
      if (line == "OK BRIDGE_READY") return true;
      if (line.startsWith("OK BRIDGE_SLEEP")) continue;
      if (line.startsWith("ERR")) return false;
    }
  }

  return false;
}

bool bridgeExpectLine(const String &cmd, String *responseOut = nullptr) {
  bridgeSendLine(cmd);

  String line;
  if (!bridgeReadLine(line, MOTH_LINE_TIMEOUT_MS)) {
    Serial.println("No AudioMoth response.");
    return false;
  }

  if (responseOut) *responseOut = line;
  return true;
}

bool bridgeExpectOk(const String &cmd, String *responseOut = nullptr) {
  String line;
  if (!bridgeExpectLine(cmd, &line)) return false;
  if (responseOut) *responseOut = line;
  return line.startsWith("OK");
}

bool openBridgeSession() {
  if (bridgeOpen) return true;

  Serial.println();
  Serial.println("Opening bridge session...");
  Serial.printf("Initial MOTH_BUSY=%d\n", mothBusy() ? 1 : 0);

  // Flush only before asserting ESP_REQ. AudioMoth sends OK BRIDGE_READY
  // immediately after it enters ESPBridge_serviceUntil(); flushing after
  // MOTH_BUSY drops can erase the only READY line.
  bridgeFlushInput();
  mothRequest(true);
  delay(20);

  Serial.printf("ESP_REQ=1, MOTH_BUSY=%d\n", mothBusy() ? 1 : 0);

  if (mothBusy() && !waitForMothIdle(MOTH_BUSY_WAIT_MS)) {
    Serial.println("FAIL: MOTH_BUSY stayed high after ESP_REQ.");
    mothRequest(false);
    bridgeFlushInput();
    return false;
  }

  Serial.println("MOTH_BUSY is low; waiting for OK BRIDGE_READY...");

  if (!bridgeWaitReady(MOTH_READY_TIMEOUT_MS)) {
    Serial.println("FAIL: AudioMoth did not send OK BRIDGE_READY.");
    mothRequest(false);
    bridgeFlushInput();
    return false;
  }

  bridgeOpen = true;
  return true;
}

void closeBridgeSession() {
  if (bridgeOpen) {
    (void)bridgeExpectOk("DONE");
  }

  bridgeFlushInput();
  mothRequest(false);
  bridgeOpen = false;
  Serial.println("Bridge closed: ESP_REQ=0");
}

bool bridgePing() {
  return bridgeExpectOk("PING");
}

bool bridgeStatus() {
  String status;
  bool ok = bridgeExpectOk("STATUS", &status);
  if (ok) {
    Serial.print("STATUS response: ");
    Serial.println(status);
  }
  return ok;
}

bool bridgeSetTime(uint32_t epoch) {
  String cmd = "TIME " + String(epoch) + " 0";
  return bridgeExpectOk(cmd);
}

bool bridgeList() {
  bridgeSendLine("LIST");

  bool ok = false;
  String line;
  uint32_t start = millis();

  while (millis() - start < 15000) {
    if (!bridgeReadLine(line, 1000)) continue;

    if (line == "END") return ok;
    if (line.startsWith("FILE ")) {
      ok = true;
      continue;
    }
    if (line.startsWith("ERR ")) {
      Serial.println("LIST failed. This is expected if AudioMoth uploadAllowed=0.");
      return false;
    }
  }

  Serial.println("LIST timeout.");
  return false;
}

bool runFullTest(uint32_t epoch) {
  if (!openBridgeSession()) return false;

  bool ok = true;
  ok = ok && bridgePing();
  ok = ok && bridgeSetTime(epoch);
  ok = ok && bridgeStatus();

  closeBridgeSession();
  return ok;
}

String readSerialCommand() {
  static String buffer;

  while (Serial.available()) {
    char c = static_cast<char>(Serial.read());
    if (c == '\r') continue;
    if (c == '\n') {
      String out = buffer;
      buffer = "";
      out.trim();
      return out;
    }
    buffer += c;
  }

  return "";
}

void printHelp() {
  Serial.println();
  Serial.println("Commands:");
  Serial.println("  open");
  Serial.println("  ping");
  Serial.println("  status");
  Serial.println("  time <epoch>");
  Serial.println("  list");
  Serial.println("  done");
  Serial.println("  test <epoch>");
  Serial.println();
}

void setup() {
  Serial.begin(SERIAL_BAUD);
  delay(500);

  pinMode(PIN_MOTH_REQ, OUTPUT);
  digitalWrite(PIN_MOTH_REQ, LOW);

  pinMode(PIN_MOTH_BUSY, INPUT);

  MothSerial.begin(MOTH_UART_BAUD, SERIAL_8N1, PIN_MOTH_UART_RX, PIN_MOTH_UART_TX);
  bridgeFlushInput();

  Serial.println();
  Serial.println("AudioMoth ESPBridge Diagnostic - Option A");
  Serial.println("GPS spoofing is not used. a7=ESP_REQ, a8=MOTH_BUSY.");
  Serial.printf("UART baud: %lu\n", static_cast<unsigned long>(MOTH_UART_BAUD));
  Serial.printf("Pins: RX=%d TX=%d REQ=%d BUSY=%d\n", PIN_MOTH_UART_RX, PIN_MOTH_UART_TX, PIN_MOTH_REQ, PIN_MOTH_BUSY);
  Serial.printf("MOTH_BUSY=%d\n", mothBusy() ? 1 : 0);
  printHelp();
}

void loop() {
  String cmd = readSerialCommand();
  if (cmd.length() == 0) return;

  if (cmd == "help") {
    printHelp();
  } else if (cmd == "open") {
    Serial.println(openBridgeSession() ? "OPEN OK" : "OPEN FAILED");
  } else if (cmd == "ping") {
    if (!bridgeOpen && !openBridgeSession()) return;
    Serial.println(bridgePing() ? "PING OK" : "PING FAILED");
  } else if (cmd == "status") {
    if (!bridgeOpen && !openBridgeSession()) return;
    Serial.println(bridgeStatus() ? "STATUS OK" : "STATUS FAILED");
  } else if (cmd.startsWith("time ")) {
    uint32_t epoch = static_cast<uint32_t>(strtoul(cmd.substring(5).c_str(), nullptr, 10));
    if (!bridgeOpen && !openBridgeSession()) return;
    Serial.println(bridgeSetTime(epoch) ? "TIME OK" : "TIME FAILED");
  } else if (cmd == "list") {
    if (!bridgeOpen && !openBridgeSession()) return;
    Serial.println(bridgeList() ? "LIST OK" : "LIST FAILED");
  } else if (cmd == "done") {
    closeBridgeSession();
  } else if (cmd.startsWith("test ")) {
    uint32_t epoch = static_cast<uint32_t>(strtoul(cmd.substring(5).c_str(), nullptr, 10));
    Serial.println(runFullTest(epoch) ? "TEST OK" : "TEST FAILED");
  } else {
    Serial.println("Unknown command. Type help.");
  }
}
