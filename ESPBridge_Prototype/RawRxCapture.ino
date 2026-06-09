static volatile bool rawRxCaptureEnabled = false;
static volatile uint32_t rawRxEdgeCount = 0;
static volatile uint32_t rawRxOverflowCount = 0;
static volatile uint32_t rawRxInitialTimeUs = 0;
static volatile uint8_t rawRxInitialLevel = HIGH;
static volatile uint32_t rawRxEdgeTimesUs[RAW_RX_CAPTURE_EDGES];
static volatile uint8_t rawRxEdgeLevels[RAW_RX_CAPTURE_EDGES];
static bool rawRxInterruptAttached = false;

static void IRAM_ATTR rawRxEdgeIsr() {
  if (!rawRxCaptureEnabled) return;

  uint32_t index = rawRxEdgeCount;
  if (index >= RAW_RX_CAPTURE_EDGES) {
    rawRxOverflowCount += 1;
    return;
  }

  rawRxEdgeTimesUs[index] = micros();
  rawRxEdgeLevels[index] = digitalRead(mothUartRxPin) ? HIGH : LOW;
  rawRxEdgeCount = index + 1;
}

void startRawRxCapture() {
  if (rawRxInterruptAttached) {
    detachInterrupt(digitalPinToInterrupt(mothUartRxPin));
    rawRxInterruptAttached = false;
  }

  noInterrupts();
  rawRxEdgeCount = 0;
  rawRxOverflowCount = 0;
  rawRxInitialTimeUs = micros();
  rawRxInitialLevel = digitalRead(mothUartRxPin) ? HIGH : LOW;
  rawRxCaptureEnabled = true;
  interrupts();

  attachInterrupt(digitalPinToInterrupt(mothUartRxPin), rawRxEdgeIsr, CHANGE);
  rawRxInterruptAttached = true;
}

void stopRawRxCapture() {
  if (rawRxInterruptAttached) {
    detachInterrupt(digitalPinToInterrupt(mothUartRxPin));
    rawRxInterruptAttached = false;
  }

  noInterrupts();
  rawRxCaptureEnabled = false;
  interrupts();
}

uint8_t rawLevelAtTime(const uint32_t *times, const uint8_t *levels, uint32_t count, uint8_t initialLevel, uint32_t sampleTimeUs) {
  uint8_t level = initialLevel;

  for (uint32_t i = 0; i < count; i += 1) {
    if (times[i] > sampleTimeUs) break;
    level = levels[i];
  }

  return level;
}

String sanitizeDecodedText(const String &input) {
  String out;

  for (uint32_t i = 0; i < input.length() && out.length() < 120; i += 1) {
    char c = input.charAt(i);
    if (c == '\n') {
      out += "\\n";
    } else if (c >= 32 && c <= 126) {
      out += c;
    } else {
      out += '.';
    }
  }

  return out;
}

String decodeRawRxAtBaud(const uint32_t *times, const uint8_t *levels, uint32_t count, uint8_t initialLevel, uint32_t baud) {
  if (count < 2 || baud == 0) return "";

  uint32_t bitUs = (1000000UL + baud / 2) / baud;
  uint32_t nextAllowedStart = 0;
  uint8_t previousLevel = initialLevel;
  String decoded;

  for (uint32_t edge = 0; edge < count && decoded.length() < 120; edge += 1) {
    uint32_t startUs = times[edge];
    uint8_t level = levels[edge];

    if (previousLevel == HIGH && level == LOW && startUs >= nextAllowedStart) {
      uint32_t startSampleUs = startUs + bitUs / 2;
      if (rawLevelAtTime(times, levels, count, initialLevel, startSampleUs) != LOW) {
        previousLevel = level;
        continue;
      }

      uint8_t value = 0;
      for (uint32_t bit = 0; bit < 8; bit += 1) {
        uint32_t sampleUs = startUs + bitUs + bitUs / 2 + bit * bitUs;
        if (rawLevelAtTime(times, levels, count, initialLevel, sampleUs) == HIGH) {
          value |= (1U << bit);
        }
      }

      uint32_t stopSampleUs = startUs + bitUs + bitUs / 2 + 8 * bitUs;
      if (rawLevelAtTime(times, levels, count, initialLevel, stopSampleUs) == HIGH) {
        decoded += (char)value;
      } else {
        decoded += (char)0x1A;
      }

      nextAllowedStart = startUs + 9 * bitUs;
    }

    previousLevel = level;
  }

  return decoded;
}

bool printRawRxCaptureSummary() {
  static uint32_t times[RAW_RX_CAPTURE_EDGES];
  static uint8_t levels[RAW_RX_CAPTURE_EDGES];

  uint32_t count;
  uint32_t overflow;
  uint32_t initialTimeUs;
  uint8_t initialLevel;

  noInterrupts();
  count = rawRxEdgeCount;
  if (count > RAW_RX_CAPTURE_EDGES) count = RAW_RX_CAPTURE_EDGES;
  overflow = rawRxOverflowCount;
  initialTimeUs = rawRxInitialTimeUs;
  initialLevel = rawRxInitialLevel;
  for (uint32_t i = 0; i < count; i += 1) {
    times[i] = rawRxEdgeTimesUs[i];
    levels[i] = rawRxEdgeLevels[i];
  }
  interrupts();

  Serial.println();
  Serial.println("Raw RX timing summary:");
  Serial.printf("  Captured edges: %lu\n", (unsigned long)count);
  Serial.printf("  Capture overflow: %s (%lu)\n", overflow ? "YES" : "NO", (unsigned long)overflow);

  if (count == 0) {
    Serial.printf("  Initial RX level: %u\n", initialLevel ? 1 : 0);
    return false;
  }

  uint32_t minIntervalUs = UINT32_MAX;
  uint32_t maxIntervalUs = 0;
  uint64_t shortIntervalSumUs = 0;
  uint32_t shortIntervalCount = 0;

  for (uint32_t i = 1; i < count; i += 1) {
    uint32_t intervalUs = times[i] - times[i - 1];
    if (intervalUs < minIntervalUs) minIntervalUs = intervalUs;
    if (intervalUs > maxIntervalUs) maxIntervalUs = intervalUs;
    if (intervalUs >= 20 && intervalUs <= 5000) {
      shortIntervalSumUs += intervalUs;
      shortIntervalCount += 1;
    }
  }

  uint32_t firstDeltaUs = times[0] - initialTimeUs;
  uint32_t spanUs = times[count - 1] - times[0];
  Serial.printf("  Initial RX level: %u\n", initialLevel ? 1 : 0);
  Serial.printf("  First edge after: %lu us\n", (unsigned long)firstDeltaUs);
  Serial.printf("  Edge span: %lu us\n", (unsigned long)spanUs);
  if (count > 1) {
    Serial.printf("  Edge interval min/max: %lu/%lu us\n", (unsigned long)minIntervalUs, (unsigned long)maxIntervalUs);
  }
  if (shortIntervalCount > 0) {
    uint32_t averageShortUs = (uint32_t)(shortIntervalSumUs / shortIntervalCount);
    Serial.printf("  Short interval avg: %lu us over %lu intervals\n",
                  (unsigned long)averageShortUs,
                  (unsigned long)shortIntervalCount);
    Serial.printf("  Min-interval baud estimate: %lu baud\n", (unsigned long)(1000000UL / minIntervalUs));
  }

  uint32_t bauds[] = {1200, 2400, 4800, 9600, 19200, 38400, 57600, 115200};
  bool sawBridgeText = false;

  for (uint32_t i = 0; i < sizeof(bauds) / sizeof(bauds[0]); i += 1) {
    String decoded = decodeRawRxAtBaud(times, levels, count, initialLevel, bauds[i]);
    String clean = sanitizeDecodedText(decoded);
    if (clean.length() == 0) continue;

    bool useful = clean.indexOf("OK") >= 0 || clean.indexOf("BRIDGE") >= 0 || clean.indexOf("PONG") >= 0;
    if (useful || bauds[i] == MOTH_UART_BAUD) {
      Serial.printf("  Decode @ %lu: %s\n", (unsigned long)bauds[i], clean.c_str());
    }

    if (decoded.indexOf("OK BRIDGE_READY") >= 0 || decoded.indexOf("OK PONG") >= 0) {
      sawBridgeText = true;
    }
  }

  Serial.printf("  Raw GPIO decode saw bridge text: %s\n", sawBridgeText ? "YES" : "NO");
  return sawBridgeText;
}

void commandRxDiag(uint32_t seconds) {
  Serial.printf("Raw RX diagnostic for %lu second(s).\n", (unsigned long)seconds);
  Serial.println("Holding ESP_REQ high and capturing GPIO-level RX edges.");

  flushMothInput();
  startRawRxCapture();
  setRequest(true);

  uint32_t durationMs = seconds * 1000UL;
  uint32_t start = millis();
  uint32_t lastPingMs = 0;

  while (millis() - start < durationMs) {
    uint32_t elapsed = millis() - start;
    if (elapsed - lastPingMs >= READY_PROBE_INTERVAL_MS) {
      sendMothLine("PING");
      lastPingMs = elapsed;
    }
    delay(10);
  }

  setRequest(false);
  delay(25);
  stopRawRxCapture();
  printRawRxCaptureSummary();
  flushMothInput();
  Serial.println("Raw RX diagnostic complete.");
}
