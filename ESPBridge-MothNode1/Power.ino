void initPowerPins() {
  pinMode(PIN_CHRG, INPUT);
  pinMode(PIN_DONE, INPUT);
  pinMode(PIN_BATTERY_ADC, INPUT);
  analogReadResolution(12);
  analogSetPinAttenuation(PIN_BATTERY_ADC, ADC_11db);
}

float readBatteryVoltage() {
  for (uint8_t i = 0; i < BATTERY_SETTLE_READS; i++) {
    analogReadMilliVolts(PIN_BATTERY_ADC);
    delay(BATTERY_SAMPLE_DELAY_MS);
  }

  uint32_t totalMv = 0;
  uint32_t lowestMv = UINT32_MAX;
  uint32_t highestMv = 0;
  for (uint8_t i = 0; i < BATTERY_SAMPLE_COUNT; i++) {
    uint32_t mv = analogReadMilliVolts(PIN_BATTERY_ADC);
    totalMv += mv;
    if (mv < lowestMv) lowestMv = mv;
    if (mv > highestMv) highestMv = mv;
    delay(BATTERY_SAMPLE_DELAY_MS);
  }

  // Drop one high and one low conversion so a brief ADC/current transient
  // cannot become the reported resting battery value.
  uint32_t trimmedMv = totalMv - lowestMv - highestMv;
  float averageMv = trimmedMv / (float)(BATTERY_SAMPLE_COUNT - 2);
  return (averageMv / 1000.0f) * BATTERY_DIVIDER_RATIO * BATTERY_CAL_FACTOR;
}

PowerState readPowerState() {
  PowerState p;
  p.batteryV = readBatteryVoltage();
  p.batteryPercent = estimateBatteryPercent(p.batteryV);
  p.charging = digitalRead(PIN_CHRG) == HIGH;
  p.chargeDone = digitalRead(PIN_DONE) == HIGH;
  return p;
}

float estimateBatteryPercent(float v) {
  if (v <= 3.30f) return 0.0f;
  if (v >= 4.15f) return 100.0f;
  if (v < 3.50f) return (v - 3.30f) / 0.20f * 10.0f;
  if (v < 3.70f) return 10.0f + (v - 3.50f) / 0.20f * 30.0f;
  if (v < 3.90f) return 40.0f + (v - 3.70f) / 0.20f * 35.0f;
  return 75.0f + (v - 3.90f) / 0.25f * 25.0f;
}

bool powerAllowsWiFi(const PowerState &p) {
  return p.batteryV >= MIN_WIFI_BATTERY_V;
}

bool powerAllowsUpload(const PowerState &p, bool forced) {
  if (p.batteryV < MIN_UPLOAD_BATTERY_V) return false;
  if (forced) return true;
#if REQUIRE_CHARGING_FOR_AUTO_UPLOAD
  return p.charging || p.chargeDone;
#else
  return true;
#endif
}
