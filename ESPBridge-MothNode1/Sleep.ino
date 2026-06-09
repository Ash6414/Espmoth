void prepareWakePins() {
  pinMode(PIN_MOTH_BUSY, INPUT);
  pinMode(PIN_MOTH_REQ, OUTPUT);
  digitalWrite(PIN_MOTH_REQ, LOW);

  rtc_gpio_pulldown_dis((gpio_num_t)PIN_MOTH_BUSY);
  rtc_gpio_pullup_dis((gpio_num_t)PIN_MOTH_BUSY);
}

void deepSleepMinutes(uint32_t minutes) {
  Serial.printf("Sleeping for %lu minute(s)\n", (unsigned long)minutes);
  Serial.flush();

  mothRequest(false);

  WiFi.disconnect(true);
  WiFi.mode(WIFI_OFF);
  btStop();

  prepareWakePins();
  esp_sleep_enable_timer_wakeup((uint64_t)minutes * 60ULL * 1000000ULL);
  esp_deep_sleep_start();
}
