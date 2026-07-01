String hexFromBytes(const uint8_t *data, size_t len) {
  static const char *hex = "0123456789abcdef";
  String out;
  out.reserve(len * 2);
  for (size_t i = 0; i < len; i++) {
    out += hex[(data[i] >> 4) & 0x0F];
    out += hex[data[i] & 0x0F];
  }
  return out;
}

String sha256Hex(const uint8_t *data, size_t len) {
  uint8_t hash[32];
  mbedtls_sha256_context ctx;
  mbedtls_sha256_init(&ctx);
  mbedtls_sha256_starts(&ctx, 0);
  mbedtls_sha256_update(&ctx, data, len);
  mbedtls_sha256_finish(&ctx, hash);
  mbedtls_sha256_free(&ctx);
  return hexFromBytes(hash, 32);
}

String sha256Hex(const String &s) {
  return sha256Hex((const uint8_t *)s.c_str(), s.length());
}

String hmacSha256Hex(const String &key, const String &message) {
  uint8_t out[32];
  const mbedtls_md_info_t *info = mbedtls_md_info_from_type(MBEDTLS_MD_SHA256);
  mbedtls_md_context_t ctx;
  mbedtls_md_init(&ctx);
  mbedtls_md_setup(&ctx, info, 1);
  mbedtls_md_hmac_starts(&ctx, (const unsigned char *)key.c_str(), key.length());
  mbedtls_md_hmac_update(&ctx, (const unsigned char *)message.c_str(), message.length());
  mbedtls_md_hmac_finish(&ctx, out);
  mbedtls_md_free(&ctx);
  return hexFromBytes(out, 32);
}

String randomNonce() {
  uint32_t a = esp_random();
  uint32_t b = esp_random();
  char buf[17];
  snprintf(buf, sizeof(buf), "%08lx%08lx", (unsigned long)a, (unsigned long)b);
  return String(buf);
}

String urlEncode(const String &s) {
  const char *hex = "0123456789ABCDEF";
  String out;
  out.reserve(s.length() * 3);
  for (size_t i = 0; i < s.length(); i++) {
    uint8_t c = (uint8_t)s[i];
    if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.' || c == '~') {
      out += (char)c;
    } else {
      out += '%';
      out += hex[(c >> 4) & 0x0F];
      out += hex[c & 0x0F];
    }
  }
  return out;
}

String pathWithoutQuery(const String &pathAndQuery) {
  int query = pathAndQuery.indexOf('?');
  return query >= 0 ? pathAndQuery.substring(0, query) : pathAndQuery;
}

WiFiClient uploadPlainClient;
WiFiClientSecure uploadSecureClient;
HTTPClient uploadHttpClient;
bool uploadTlsConfigured = false;
String gActiveBaseUrl;

void closeUploadHttpClient() {
  uploadHttpClient.end();
  uploadPlainClient.stop();
  uploadSecureClient.stop();
}

String normalizeRuntimeBaseUrl(String value) {
  value.trim();
  while (value.endsWith("/")) value.remove(value.length() - 1);
  return value;
}

const String &activeBaseUrl() {
  if (gActiveBaseUrl.length() == 0) {
    gActiveBaseUrl = normalizeRuntimeBaseUrl(cfgBaseUrl());
  }
  return gActiveBaseUrl;
}

bool beginUploadHttpClient(const String &url, bool &connectionReused) {
  bool secure = url.startsWith("https://");
  connectionReused = secure ? uploadSecureClient.connected() : uploadPlainClient.connected();

  if (secure) {
    if (estimatedEpochUtc() <= 1700000000UL) {
      Serial.println("Refusing HTTPS upload without a valid clock");
      return false;
    }
    if (!uploadTlsConfigured) {
      uploadSecureClient.setCACert(TLS_ROOT_CA);
      uploadSecureClient.setHandshakeTimeout(max(1UL, HTTP_TIMEOUT_MS / 1000UL));
      uploadTlsConfigured = true;
    }
    return uploadHttpClient.begin(uploadSecureClient, url);
  }

  return uploadHttpClient.begin(uploadPlainClient, url);
}

bool connectWiFi() {
  WiFi.mode(WIFI_STA);
  if (!beginWiFiConnection(cfgWifiSsid(), cfgWifiSecurity(), cfgWifiIdentity(), cfgWifiUsername(), cfgWifiPassword())) {
    Serial.println("Wi-Fi configuration failed");
    return false;
  }

  uint32_t start = millis();
  Serial.print("Connecting Wi-Fi");
  while (WiFi.status() != WL_CONNECTED && millis() - start < WIFI_CONNECT_TIMEOUT_MS) {
    delay(250);
    Serial.print('.');
  }
  Serial.println();

  if (WiFi.status() == WL_CONNECTED) {
    Serial.print("Wi-Fi connected. IP: ");
    Serial.println(WiFi.localIP());
    Serial.printf("RSSI: %d dBm\n", WiFi.RSSI());
    if (cfgBaseUrl().startsWith("https://") && estimatedEpochUtc() <= 1700000000UL && !syncClockFromNtp()) {
      Serial.println("Secure server requires valid internet time; NTP sync failed");
      return false;
    }
    return true;
  }

  Serial.println("Wi-Fi failed");
  return false;
}

bool ensureHttpNetworkReady() {
  if (WiFi.status() == WL_CONNECTED) return true;

  Serial.println("Wi-Fi disconnected before HTTP request; reconnecting");
  closeUploadHttpClient();
  WiFi.disconnect(false);
  delay(250);
  return connectWiFi();
}

bool syncClockFromNtp() {
  configTime(0, 0, "time.cloudflare.com", "pool.ntp.org", "time.google.com");
  uint32_t start = millis();
  Serial.print("Syncing internet time");
  while (millis() - start < NTP_SYNC_TIMEOUT_MS) {
    time_t now = time(nullptr);
    if (now > 1700000000L) {
      Serial.println();
      Serial.printf("NTP epoch: %ld\n", (long)now);
      return true;
    }
    delay(250);
    Serial.print('.');
  }
  Serial.println();
  return false;
}

bool beginHttpClient(HTTPClient &http, WiFiClient &plainClient, WiFiClientSecure &secureClient, const String &url) {
  if (url.startsWith("https://")) {
    if (estimatedEpochUtc() <= 1700000000UL) {
      Serial.println("Refusing HTTPS without a valid clock");
      return false;
    }
    secureClient.setCACert(TLS_ROOT_CA);
    secureClient.setHandshakeTimeout(max(1UL, HTTP_TIMEOUT_MS / 1000UL));
    return http.begin(secureClient, url);
  }
  return http.begin(plainClient, url);
}

void syncSystemClock(uint32_t epochUtc) {
  struct timeval tv;
  tv.tv_sec = epochUtc;
  tv.tv_usec = 0;
  settimeofday(&tv, nullptr);
}

uint32_t estimatedEpochUtc() {
  time_t now = time(nullptr);
  if (now > 1700000000UL) return (uint32_t)now;
  if (bootHasFreshServerTime) return bootServerEpoch + ((millis() - bootServerMillis) / 1000UL);
  if (rtcLastServerEpoch > 1700000000UL && rtcLastSyncEspUs > 0) {
    int64_t deltaUs = esp_timer_get_time() - rtcLastSyncEspUs;
    if (deltaUs > 0) return rtcLastServerEpoch + (uint32_t)(deltaUs / 1000000LL);
  }
  return 0;
}

long getServerTimeFromBaseUrl(const String &baseUrl, const String &path, uint32_t *rttMsOut, uint32_t timeoutMs) {
  WiFiClient plainClient;
  WiFiClientSecure secureClient;
  HTTPClient http;
  String normalized = normalizeRuntimeBaseUrl(baseUrl);
  if (normalized.length() == 0) return 0;

  String url = normalized + path;

  if (!beginHttpClient(http, plainClient, secureClient, url)) {
    Serial.printf("GET %s begin failed for %s\n", path.c_str(), normalized.c_str());
    return 0;
  }
  http.setTimeout(timeoutMs);

  uint32_t t0 = millis();
  int code = http.GET();
  uint32_t rtt = millis() - t0;

  if (code != 200) {
    Serial.printf("GET %s on %s -> %d\n", path.c_str(), normalized.c_str(), code);
    http.end();
    return 0;
  }

  String resp = http.getString();
  http.end();

  StaticJsonDocument<256> doc;
  if (deserializeJson(doc, resp)) return 0;

  long epoch = doc["epoch_utc"] | (doc["server_time"] | 0);
  if (epoch <= 1700000000L) return 0;

  gActiveBaseUrl = normalized;
  if (rttMsOut) *rttMsOut = rtt;
  return epoch;
}

long getServerTime(uint32_t *rttMsOut) {
  String path = ENDPOINT_SERVER_TIME;
  String primary = normalizeRuntimeBaseUrl(cfgBaseUrl());
  String fallback = normalizeRuntimeBaseUrl(String(SERVER_FALLBACK_BASE_URL));

#if PREFER_FALLBACK_SERVER_WHEN_REACHABLE
  if (fallback.length() > 0 && fallback != primary) {
    Serial.printf("Checking LAN/fallback server %s\n", fallback.c_str());
    long fallbackEpoch = getServerTimeFromBaseUrl(fallback, path, rttMsOut, FALLBACK_SERVER_PROBE_TIMEOUT_MS);
    if (fallbackEpoch > 1700000000L) return fallbackEpoch;
    Serial.println("LAN/fallback server not reachable; trying primary server");
  }
#endif

  long epoch = getServerTimeFromBaseUrl(primary, path, rttMsOut, HTTP_TIMEOUT_MS);
  if (epoch > 1700000000L) return epoch;

  if (fallback.length() > 0 && fallback != primary) {
    Serial.printf("Primary server time failed; trying fallback %s\n", fallback.c_str());
    epoch = getServerTimeFromBaseUrl(fallback, path, rttMsOut, HTTP_TIMEOUT_MS);
    if (epoch > 1700000000L) return epoch;
  }

  return 0;
}

void addAuthHeaders(HTTPClient &http, const String &method, const String &pathAndQuery, const uint8_t *body, size_t bodyLen, long serverEpoch) {
  uint32_t estimated = estimatedEpochUtc();
  long authEpoch = estimated > 1700000000UL ? (long)estimated : serverEpoch;
  String ts = String(authEpoch);
  String nonce = randomNonce();
  String bodyHash = sha256Hex(body, bodyLen);
  String path = pathWithoutQuery(pathAndQuery);
  String canonical = method + "\n" + path + "\n" + ts + "\n" + nonce + "\n" + bodyHash;
  String sig = hmacSha256Hex(cfgDeviceSecret(), canonical);

  http.addHeader("X-Node-ID", cfgNodeId());
  http.addHeader("X-Key-ID", cfgKeyId());
  http.addHeader("X-Timestamp", ts);
  http.addHeader("X-Nonce", nonce);
  http.addHeader("X-Body-SHA256", bodyHash);
  http.addHeader("X-Signature", sig);
}

bool signedPostJson(const String &path, const String &body, long serverEpoch, String &responseOut) {
  for (uint8_t attempt = 1; attempt <= 2; attempt++) {
    if (!ensureHttpNetworkReady()) return false;

    WiFiClient plainClient;
    WiFiClientSecure secureClient;
    HTTPClient http;
    String url = activeBaseUrl() + path;
    if (!beginHttpClient(http, plainClient, secureClient, url)) {
      Serial.printf("POST %s begin failed%s\n", path.c_str(), attempt == 1 ? " (will retry)" : "");
      closeUploadHttpClient();
      delay(500);
      continue;
    }

    http.setTimeout(HTTP_TIMEOUT_MS);
    http.addHeader("Content-Type", "application/json");
    addAuthHeaders(http, "POST", path, (const uint8_t *)body.c_str(), body.length(), serverEpoch);

    int code = http.POST(body);
    responseOut = http.getString();
    http.end();

    Serial.printf("POST %s -> %d%s\n", path.c_str(), code, (code < 0 && attempt == 1) ? " (will retry)" : "");
#if DEBUG_HTTP_RESPONSES
    if (responseOut.length()) Serial.println(responseOut);
#endif
    if (code >= 200 && code < 300) return true;
    if (code >= 400 && code < 500) return false;

    closeUploadHttpClient();
    delay(500);
  }

  return false;
}

bool signedGet(const String &path, long serverEpoch, String &responseOut) {
  for (uint8_t attempt = 1; attempt <= 2; attempt++) {
    if (!ensureHttpNetworkReady()) return false;

    WiFiClient plainClient;
    WiFiClientSecure secureClient;
    HTTPClient http;
    String url = activeBaseUrl() + path;
    if (!beginHttpClient(http, plainClient, secureClient, url)) {
      Serial.printf("GET %s begin failed%s\n", path.c_str(), attempt == 1 ? " (will retry)" : "");
      closeUploadHttpClient();
      delay(500);
      continue;
    }

    http.setTimeout(HTTP_TIMEOUT_MS);
    addAuthHeaders(http, "GET", path, (const uint8_t *)"", 0, serverEpoch);

    int code = http.GET();
    responseOut = http.getString();
    http.end();

    Serial.printf("GET %s -> %d%s\n", path.c_str(), code, (code < 0 && attempt == 1) ? " (will retry)" : "");
#if DEBUG_HTTP_RESPONSES
    if (responseOut.length()) Serial.println(responseOut);
#endif
    if (code >= 200 && code < 300) return true;
    if (code >= 400 && code < 500) return false;

    closeUploadHttpClient();
    delay(500);
  }

  return false;
}

bool signedPostBinary(const String &pathAndQuery, const uint8_t *body, size_t bodyLen, long serverEpoch, String &responseOut) {
  WiFiClient plainClient;
  WiFiClientSecure secureClient;
  HTTPClient http;
  String url = activeBaseUrl() + pathAndQuery;
  if (!beginHttpClient(http, plainClient, secureClient, url)) return false;

  http.setTimeout(HTTP_TIMEOUT_MS);
  http.addHeader("Content-Type", "application/octet-stream");
  addAuthHeaders(http, "POST", pathAndQuery, body, bodyLen, serverEpoch);

  int code = http.POST((uint8_t *)body, bodyLen);
  responseOut = http.getString();
  http.end();

  if (code < 200 || code >= 300) {
    Serial.printf("POST(binary) %s bytes=%u -> %d\n", pathAndQuery.c_str(), (unsigned)bodyLen, code);
  }
#if DEBUG_HTTP_RESPONSES
  if (responseOut.length()) Serial.println(responseOut);
#endif
  return code >= 200 && code < 300;
}

bool signedPutBinary(const String &pathAndQuery, const uint8_t *body, size_t bodyLen, long serverEpoch, String &responseOut) {
  String url = activeBaseUrl() + pathAndQuery;
  bool connectionReused = false;
  if (!beginUploadHttpClient(url, connectionReused)) return false;

  uploadHttpClient.setReuse(true);
  uploadHttpClient.setTimeout(HTTP_TIMEOUT_MS);
  uploadHttpClient.addHeader("Content-Type", "application/octet-stream");
  uint32_t authStartMs = millis();
  addAuthHeaders(uploadHttpClient, "PUT", pathAndQuery, body, bodyLen, serverEpoch);
  uint32_t authMs = millis() - authStartMs;

  uint32_t requestStartMs = millis();
  int code = uploadHttpClient.sendRequest("PUT", (uint8_t *)body, bodyLen);
  responseOut = uploadHttpClient.getString();
  uint32_t requestMs = millis() - requestStartMs;
  uploadHttpClient.end();

  Serial.printf("PUT chunk bytes=%u -> %d in %lu ms (auth=%lu ms, connection=%s)\n",
                (unsigned)bodyLen, code, (unsigned long)requestMs, (unsigned long)authMs,
                connectionReused ? "reused" : "new");

  if (code < 200 || code >= 300) {
    uploadPlainClient.stop();
    uploadSecureClient.stop();
  }
#if DEBUG_HTTP_RESPONSES
  if (responseOut.length()) Serial.println(responseOut);
#endif
  return code >= 200 && code < 300;
}
