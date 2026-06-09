/*
  Clean ESP32 <-> AudioMoth ESPBridge bring-up prototype.

  Purpose:
    Prove the physical and firmware bridge before adding Wi-Fi/server upload code.

  AudioMoth requirements:
    - AudioMoth is flashed with the ESPBridge firmware.
    - AudioMoth switch is in CUSTOM mode.
    - GPS time setting is disabled because a7/a8 are bridge pins.
*/

#include <Arduino.h>
#include "Config.h"
#include "ProbeEdges.h"

HardwareSerial MothSerial(2);

bool bridgeOpen = false;
bool mothUartSwapped = false;
String serialCommand;
String mothLineBuffer;
uint32_t mothRxBytesSeen = 0;
int mothUartRxPin = PIN_MOTH_UART_RX;
int mothUartTxPin = PIN_MOTH_UART_TX;

void printHelp();
void printPins();
void initBridgePins();
void configureMothUart(bool swapped);
void resetMothParser();
uint32_t mothRxByteCount();
uint32_t mothPartialByteCount();
void flushMothInput();
void setRequest(bool state);
bool mothBusy();
bool readMothLine(String &line, uint32_t timeoutMs);
void sendMothLine(const String &line);
bool waitForBusyLow(uint32_t timeoutMs);
bool waitForReadyOrPong(uint32_t timeoutMs);
bool openBridge();
void closeBridge();
bool expectOk(const String &command, String *responseOut = nullptr);
void commandStatus();
void commandList();
void commandSetTime(const String &arg);
void commandReqProbe(uint32_t seconds);
void commandRxDiag(uint32_t seconds);
void commandWatchPins(uint32_t seconds);
void commandSwapProbe(uint32_t seconds);
void handleCommand(String command);
void runBootProbe();
void startRawRxCapture();
void stopRawRxCapture();
bool printRawRxCaptureSummary();

void setup() {
  Serial.begin(SERIAL_BAUD);
  delay(300);

  initBridgePins();

  Serial.println();
  Serial.println("=== ESPBridge clean prototype ===");
  Serial.println("AudioMoth must be in CUSTOM switch position.");
  printPins();
  printHelp();

#if AUTO_PROBE_ON_BOOT
  runBootProbe();
#endif
}

void loop() {
  while (Serial.available()) {
    char c = (char)Serial.read();
    if (c == '\r') continue;
    if (c == '\n') {
      serialCommand.trim();
      if (serialCommand.length()) handleCommand(serialCommand);
      serialCommand = "";
    } else if (serialCommand.length() < 160) {
      serialCommand += c;
    }
  }

  String line;
  if (readMothLine(line, 1)) {
    Serial.print("MOTH->ESP ");
    Serial.println(line);
  }
}
