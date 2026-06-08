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

bool connectWiFi() {
  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);

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
    return true;
  }

  Serial.println("Wi-Fi failed");
  return false;
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

long getServerTime(uint32_t *rttMsOut) {
  WiFiClient client;
  HTTPClient http;
  String path = ENDPOINT_SERVER_TIME;
  String url = String(BASE_URL) + path;

  if (!http.begin(client, url)) return 0;
  http.setTimeout(HTTP_TIMEOUT_MS);

  uint32_t t0 = millis();
  int code = http.GET();
  uint32_t rtt = millis() - t0;

  if (code != 200) {
    http.end();
    return 0;
  }

  String resp = http.getString();
  http.end();

  StaticJsonDocument<256> doc;
  if (deserializeJson(doc, resp)) return 0;

  if (rttMsOut) *rttMsOut = rtt;
  return doc["epoch_utc"] | 0;
}

void addAuthHeaders(HTTPClient &http, const String &method, const String &path, const uint8_t *body, size_t bodyLen, long serverEpoch) {
  String ts = String(serverEpoch > 0 ? serverEpoch : (long)estimatedEpochUtc());
  String nonce = randomNonce();
  String bodyHash = sha256Hex(body, bodyLen);
  String canonical = method + "\n" + path + "\n" + ts + "\n" + nonce + "\n" + bodyHash;
  String sig = hmacSha256Hex(DEVICE_SECRET, canonical);

  http.addHeader("X-Node-ID", NODE_ID);
  http.addHeader("X-Key-ID", KEY_ID);
  http.addHeader("X-Timestamp", ts);
  http.addHeader("X-Nonce", nonce);
  http.addHeader("X-Body-SHA256", bodyHash);
  http.addHeader("X-Signature", sig);
}

bool signedPostJson(const String &path, const String &body, long serverEpoch, String &responseOut) {
  WiFiClient client;
  HTTPClient http;
  String url = String(BASE_URL) + path;
  if (!http.begin(client, url)) return false;

  http.setTimeout(HTTP_TIMEOUT_MS);
  http.addHeader("Content-Type", "application/json");
  addAuthHeaders(http, "POST", path, (const uint8_t *)body.c_str(), body.length(), serverEpoch);

  int code = http.POST(body);
  responseOut = http.getString();
  http.end();

  Serial.printf("POST %s -> %d\n", path.c_str(), code);
#if DEBUG_HTTP_RESPONSES
  if (responseOut.length()) Serial.println(responseOut);
#endif
  return code >= 200 && code < 300;
}

bool signedGet(const String &path, long serverEpoch, String &responseOut) {
  WiFiClient client;
  HTTPClient http;
  String url = String(BASE_URL) + path;
  if (!http.begin(client, url)) return false;

  http.setTimeout(HTTP_TIMEOUT_MS);
  addAuthHeaders(http, "GET", path, (const uint8_t *)"", 0, serverEpoch);

  int code = http.GET();
  responseOut = http.getString();
  http.end();

  Serial.printf("GET %s -> %d\n", path.c_str(), code);
#if DEBUG_HTTP_RESPONSES
  if (responseOut.length()) Serial.println(responseOut);
#endif
  return code >= 200 && code < 300;
}

bool signedPostBinary(const String &pathAndQuery, const uint8_t *body, size_t bodyLen, long serverEpoch, String &responseOut) {
  WiFiClient client;
  HTTPClient http;
  String url = String(BASE_URL) + pathAndQuery;
  if (!http.begin(client, url)) return false;

  http.setTimeout(HTTP_TIMEOUT_MS);
  http.addHeader("Content-Type", "application/octet-stream");
  addAuthHeaders(http, "POST", pathAndQuery, body, bodyLen, serverEpoch);

  int code = http.POST((uint8_t *)body, bodyLen);
  responseOut = http.getString();
  http.end();

  Serial.printf("POST(binary) %s bytes=%u -> %d\n", pathAndQuery.c_str(), (unsigned)bodyLen, code);
#if DEBUG_HTTP_RESPONSES
  if (responseOut.length()) Serial.println(responseOut);
#endif
  return code >= 200 && code < 300;
}
