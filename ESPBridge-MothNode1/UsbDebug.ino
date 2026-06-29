void runUsbDebugMothStatus(long serverEpoch) {
  String status;
  bool ok = false;

  if (openBridgeSession(serverEpoch)) {
    ok = bridgeStatus(status);
    closeBridgeSession();
  }

  if (ok) {
    Serial.printf("USB_DEBUG_MOTH_STATUS %s\n", status.c_str());
  } else {
    Serial.println("USB_DEBUG_MOTH_STATUS FAIL");
  }
}

void runUsbDebugMothList(long serverEpoch) {
  MothFile files[MOTH_MAX_FILES_PER_SESSION];
  MothSdInfo sdInfo = {false, 0, 0};
  size_t fileCount = 0;

  if (!openBridgeSession(serverEpoch)) {
    Serial.println("USB_DEBUG_MOTH_LIST FAIL bridge_open");
    return;
  }

  bool ok = bridgeList(files, MOTH_MAX_FILES_PER_SESSION, fileCount, &sdInfo);
  closeBridgeSession();

  if (!ok) {
    Serial.println("USB_DEBUG_MOTH_LIST FAIL list");
    return;
  }

  Serial.printf("USB_DEBUG_MOTH_LIST count=%u sd_ok=%d free_kb=%lu total_kb=%lu\n",
                (unsigned)fileCount,
                sdInfo.valid ? 1 : 0,
                (unsigned long)sdInfo.freeKb,
                (unsigned long)sdInfo.totalKb);
  for (size_t i = 0; i < fileCount; i++) {
    Serial.printf("USB_DEBUG_MOTH_FILE path=%s size=%lu\n",
                  files[i].path.c_str(),
                  (unsigned long)files[i].size);
  }
}

void runUsbBridgeDebugWindow(long serverEpoch, uint32_t windowMs) {
  if (serverEpoch <= 1700000000L) serverEpoch = 0;

  Serial.printf("USB_DEBUG_READY window_ms=%lu commands=MOTH_STATUS,MOTH_LIST,DONE\n",
                (unsigned long)windowMs);
  Serial.setTimeout(50);

  uint32_t start = millis();
  while (millis() - start < windowMs) {
    if (!Serial.available()) {
      delay(10);
      continue;
    }

    String cmd = Serial.readStringUntil('\n');
    cmd.trim();
    cmd.toUpperCase();

    if (cmd.length() == 0) {
      continue;
    }
    if (cmd == "DONE" || cmd == "SLEEP") {
      Serial.println("USB_DEBUG_DONE");
      return;
    }
    if (cmd == "MOTH_STATUS") {
      runUsbDebugMothStatus(serverEpoch);
      start = millis();
      continue;
    }
    if (cmd == "MOTH_LIST") {
      runUsbDebugMothList(serverEpoch);
      start = millis();
      continue;
    }

    Serial.printf("USB_DEBUG_UNKNOWN %s\n", cmd.c_str());
  }

  Serial.println("USB_DEBUG_TIMEOUT");
}
