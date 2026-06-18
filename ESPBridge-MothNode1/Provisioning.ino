#include <Preferences.h>
#include <WebServer.h>

Preferences nodePrefs;
WebServer provisionServer(80);

String gWifiSsid;
String gWifiPassword;
String gBaseUrl;
String gNodeId;
String gKeyId;
String gDeviceSecret;
bool gNodeConfigReady = false;

String normalizeBaseUrl(String value) {
  value.trim();
  while (value.endsWith("/")) value.remove(value.length() - 1);
  return value;
}

String htmlEscape(const String &value) {
  String out;
  out.reserve(value.length() + 8);
  for (size_t i = 0; i < value.length(); i++) {
    char c = value[i];
    if (c == '&') out += F("&amp;");
    else if (c == '<') out += F("&lt;");
    else if (c == '>') out += F("&gt;");
    else if (c == '"') out += F("&quot;");
    else out += c;
  }
  return out;
}

String chipSuffix() {
  uint64_t mac = ESP.getEfuseMac();
  char buf[7];
  snprintf(buf, sizeof(buf), "%06lX", (unsigned long)(mac & 0xFFFFFFUL));
  return String(buf);
}

bool nodeConfigReady() {
  return gNodeConfigReady;
}

const String &cfgWifiSsid() {
  return gWifiSsid;
}

const String &cfgWifiPassword() {
  return gWifiPassword;
}

const String &cfgBaseUrl() {
  return gBaseUrl;
}

const String &cfgNodeId() {
  return gNodeId;
}

const String &cfgKeyId() {
  return gKeyId;
}

const String &cfgDeviceSecret() {
  return gDeviceSecret;
}

bool loadNodeConfig() {
  nodePrefs.begin("batnode", true);
  gWifiSsid = nodePrefs.getString("wifi_ssid", DEFAULT_WIFI_SSID);
  gWifiPassword = nodePrefs.getString("wifi_pass", DEFAULT_WIFI_PASSWORD);
  gBaseUrl = nodePrefs.getString("base_url", DEFAULT_BASE_URL);
  gNodeId = nodePrefs.getString("node_id", DEFAULT_NODE_ID);
  gKeyId = nodePrefs.getString("key_id", DEFAULT_KEY_ID);
  gDeviceSecret = nodePrefs.getString("secret", DEFAULT_DEVICE_SECRET);
  nodePrefs.end();

  gWifiSsid.trim();
  gBaseUrl = normalizeBaseUrl(gBaseUrl);
  gNodeId.trim();
  gKeyId.trim();
  gDeviceSecret.trim();

  gNodeConfigReady = gWifiSsid.length() > 0 &&
                     gBaseUrl.length() > 0 &&
                     gNodeId.length() > 0 &&
                     gKeyId.length() > 0 &&
                     gDeviceSecret.length() >= 32;
  return gNodeConfigReady;
}

bool saveNodeConfig(const String &wifiSsid,
                    const String &wifiPassword,
                    const String &baseUrl,
                    const String &nodeId,
                    const String &keyId,
                    const String &deviceSecret) {
  String ssid = wifiSsid;
  String url = normalizeBaseUrl(baseUrl);
  String nid = nodeId;
  String kid = keyId;
  String secret = deviceSecret;
  ssid.trim();
  nid.trim();
  kid.trim();
  secret.trim();

  if (ssid.length() == 0 || url.length() == 0 || nid.length() == 0 || kid.length() == 0 || secret.length() < 32) {
    return false;
  }

  nodePrefs.begin("batnode", false);
  nodePrefs.putString("wifi_ssid", ssid);
  nodePrefs.putString("wifi_pass", wifiPassword);
  nodePrefs.putString("base_url", url);
  nodePrefs.putString("node_id", nid);
  nodePrefs.putString("key_id", kid);
  nodePrefs.putString("secret", secret);
  nodePrefs.end();

  gWifiSsid = ssid;
  gWifiPassword = wifiPassword;
  gBaseUrl = url;
  gNodeId = nid;
  gKeyId = kid;
  gDeviceSecret = secret;
  gNodeConfigReady = true;
  return true;
}

bool provisioningForced() {
#if PROVISION_FORCE_PIN >= 0
  pinMode(PROVISION_FORCE_PIN, INPUT_PULLUP);
  delay(5);
  return digitalRead(PROVISION_FORCE_PIN) == LOW;
#else
  return false;
#endif
}

String portalPage(const String &message) {
  String suffix = chipSuffix();
  String html;
  html.reserve(6500);
  html += F("<!doctype html><html><head><meta name='viewport' content='width=device-width,initial-scale=1'>");
  html += F("<title>Bat Node Setup</title><style>");
  html += F("body{font-family:system-ui,-apple-system,Segoe UI,sans-serif;margin:0;background:#f7f7f3;color:#1d1d1b}");
  html += F("main{max-width:760px;margin:auto;padding:24px}section{background:#fff;border:1px solid #ddd;border-radius:8px;padding:18px;margin:14px 0}");
  html += F("label{display:block;font-weight:650;margin-top:12px}input{width:100%;box-sizing:border-box;padding:10px;border:1px solid #bbb;border-radius:6px;font-size:16px}");
  html += F("button{margin-top:16px;padding:11px 14px;border:0;border-radius:6px;background:#145a7a;color:white;font-weight:700;font-size:16px}");
  html += F(".danger{background:#8d2c20}.msg{padding:12px;border-radius:6px;background:#e9f2ef;border:1px solid #b7d4ca}.hint{color:#555;font-size:14px}</style></head><body><main>");
  html += F("<h1>Bat Node Setup</h1>");
  html += F("<p class='hint'>Connect this node once. After credentials are saved, future boots skip this page and connect normally.</p>");
  if (message.length()) {
    html += F("<div class='msg'>");
    html += htmlEscape(message);
    html += F("</div>");
  }

  html += F("<section><h2>Automatic Provisioning</h2>");
  html += F("<form method='post' action='/provision'>");
  html += F("<label>Wi-Fi SSID</label><input name='wifi_ssid' value='");
  html += htmlEscape(gWifiSsid);
  html += F("' required>");
  html += F("<label>Wi-Fi Password</label><input name='wifi_password' type='password' value='");
  html += htmlEscape(gWifiPassword);
  html += F("'>");
  html += F("<label>Server URL</label><input name='base_url' value='");
  html += htmlEscape(gBaseUrl.length() ? gBaseUrl : String(DEFAULT_BASE_URL));
  html += F("' required>");
  html += F("<label>Provisioning Token</label><input name='provisioning_token' type='password' required>");
  html += F("<label>Node Name</label><input name='node_name' value='Bat Node ");
  html += suffix;
  html += F("'>");
  html += F("<label>Requested Node ID (optional)</label><input name='requested_node_id' placeholder='BATNODE_");
  html += suffix;
  html += F("'>");
  html += F("<button type='submit'>Provision and Save</button></form></section>");

  html += F("<section><h2>Manual Credentials</h2>");
  html += F("<form method='post' action='/save'>");
  html += F("<label>Wi-Fi SSID</label><input name='wifi_ssid' value='");
  html += htmlEscape(gWifiSsid);
  html += F("' required>");
  html += F("<label>Wi-Fi Password</label><input name='wifi_password' type='password' value='");
  html += htmlEscape(gWifiPassword);
  html += F("'>");
  html += F("<label>Server URL</label><input name='base_url' value='");
  html += htmlEscape(gBaseUrl.length() ? gBaseUrl : String(DEFAULT_BASE_URL));
  html += F("' required>");
  html += F("<label>Node ID</label><input name='node_id' value='");
  html += htmlEscape(gNodeId);
  html += F("' required>");
  html += F("<label>Key ID</label><input name='key_id' value='");
  html += htmlEscape(gKeyId.length() ? gKeyId : String("key-1"));
  html += F("' required>");
  html += F("<label>Device Secret</label><input name='device_secret' type='password' value='");
  html += htmlEscape(gDeviceSecret);
  html += F("' required>");
  html += F("<button type='submit'>Save Manual Config</button></form></section>");

  html += F("<section><h2>Reset</h2><form method='post' action='/clear'>");
  html += F("<button class='danger' type='submit'>Clear Saved Config</button></form></section>");
  html += F("</main></body></html>");
  return html;
}

void sendPortalPage(const String &message) {
  provisionServer.send(200, "text/html", portalPage(message));
}

bool waitForProvisionWiFi(const String &ssid, const String &password) {
  WiFi.mode(WIFI_AP_STA);
  WiFi.begin(ssid.c_str(), password.c_str());
  uint32_t start = millis();
  Serial.print("Provisioning Wi-Fi connect");
  while (WiFi.status() != WL_CONNECTED && millis() - start < WIFI_CONNECT_TIMEOUT_MS) {
    delay(250);
    Serial.print('.');
  }
  Serial.println();
  if (WiFi.status() == WL_CONNECTED) {
    Serial.print("Provisioning Wi-Fi connected. IP: ");
    Serial.println(WiFi.localIP());
    return true;
  }
  Serial.println("Provisioning Wi-Fi failed");
  return false;
}

bool requestServerProvisioning(const String &baseUrl,
                               const String &token,
                               const String &nodeName,
                               const String &requestedNodeId,
                               String &nodeIdOut,
                               String &keyIdOut,
                               String &secretOut,
                               String &errorOut) {
  WiFiClient client;
  HTTPClient http;
  String url = normalizeBaseUrl(baseUrl) + "/v1/provision/node";
  if (!http.begin(client, url)) {
    errorOut = "Could not open provisioning URL";
    return false;
  }
  http.setTimeout(HTTP_TIMEOUT_MS);
  http.addHeader("Content-Type", "application/json");

  StaticJsonDocument<768> doc;
  doc["provisioning_token"] = token;
  doc["node_name"] = nodeName;
  doc["node_id"] = requestedNodeId;
  doc["hardware_version"] = "ESP32 AudioMoth bridge";
  doc["firmware_version"] = "Moth_Node_ESPBridge";
  String body;
  serializeJson(doc, body);

  int code = http.POST(body);
  String response = http.getString();
  http.end();

  if (code < 200 || code >= 300) {
    errorOut = "Server returned HTTP " + String(code) + ": " + response;
    return false;
  }

  StaticJsonDocument<768> resp;
  DeserializationError err = deserializeJson(resp, response);
  if (err || !(resp["ok"] | false)) {
    errorOut = "Could not parse provisioning response";
    return false;
  }

  nodeIdOut = String((const char *)(resp["node_id"] | ""));
  keyIdOut = String((const char *)(resp["key_id"] | ""));
  secretOut = String((const char *)(resp["device_secret"] | ""));
  if (nodeIdOut.length() == 0 || keyIdOut.length() == 0 || secretOut.length() < 32) {
    errorOut = "Provisioning response was missing credentials";
    return false;
  }
  return true;
}

void handlePortalRoot() {
  sendPortalPage("");
}

void handleManualSave() {
  bool ok = saveNodeConfig(
    provisionServer.arg("wifi_ssid"),
    provisionServer.arg("wifi_password"),
    provisionServer.arg("base_url"),
    provisionServer.arg("node_id"),
    provisionServer.arg("key_id"),
    provisionServer.arg("device_secret")
  );
  if (!ok) {
    sendPortalPage("Manual save failed. Check that Wi-Fi, server URL, node ID, key ID, and secret are filled in.");
    return;
  }
  provisionServer.send(200, "text/html", "<html><body><h1>Saved</h1><p>Restarting node...</p></body></html>");
  delay(1000);
  ESP.restart();
}

void handleAutoProvision() {
  String ssid = provisionServer.arg("wifi_ssid");
  String password = provisionServer.arg("wifi_password");
  String baseUrl = normalizeBaseUrl(provisionServer.arg("base_url"));
  String token = provisionServer.arg("provisioning_token");
  String nodeName = provisionServer.arg("node_name");
  String requestedNodeId = provisionServer.arg("requested_node_id");
  ssid.trim();
  token.trim();
  requestedNodeId.trim();
  if (nodeName.length() == 0) nodeName = "Bat Node " + chipSuffix();

  if (ssid.length() == 0 || baseUrl.length() == 0 || token.length() == 0) {
    sendPortalPage("Automatic provisioning needs Wi-Fi SSID, server URL, and provisioning token.");
    return;
  }

  if (!waitForProvisionWiFi(ssid, password)) {
    sendPortalPage("Could not connect to the field Wi-Fi. Check SSID/password and try again.");
    return;
  }

  String nodeId;
  String keyId;
  String secret;
  String error;
  if (!requestServerProvisioning(baseUrl, token, nodeName, requestedNodeId, nodeId, keyId, secret, error)) {
    sendPortalPage(error);
    return;
  }

  if (!saveNodeConfig(ssid, password, baseUrl, nodeId, keyId, secret)) {
    sendPortalPage("Provisioning succeeded, but saving credentials failed.");
    return;
  }

  provisionServer.send(200, "text/html", "<html><body><h1>Provisioned</h1><p>Credentials saved. Restarting node...</p></body></html>");
  delay(1000);
  ESP.restart();
}

void handleClearConfig() {
  nodePrefs.begin("batnode", false);
  nodePrefs.clear();
  nodePrefs.end();
  provisionServer.send(200, "text/html", "<html><body><h1>Cleared</h1><p>Restarting setup portal...</p></body></html>");
  delay(1000);
  ESP.restart();
}

void runProvisioningPortal() {
  String apSsid = String(PROVISION_AP_PREFIX) + "-" + chipSuffix();
  WiFi.mode(WIFI_AP);
  bool apOk = WiFi.softAP(apSsid.c_str(), PROVISION_AP_PASSWORD);

  Serial.println();
  Serial.println("=== Bat Node setup portal ===");
  Serial.printf("AP: %s\n", apSsid.c_str());
  Serial.printf("Password: %s\n", PROVISION_AP_PASSWORD);
  Serial.print("Setup URL: http://");
  Serial.println(WiFi.softAPIP());
  if (!apOk) Serial.println("Warning: setup AP did not report success");

  provisionServer.on("/", HTTP_GET, handlePortalRoot);
  provisionServer.on("/save", HTTP_POST, handleManualSave);
  provisionServer.on("/provision", HTTP_POST, handleAutoProvision);
  provisionServer.on("/clear", HTTP_POST, handleClearConfig);
  provisionServer.onNotFound(handlePortalRoot);
  provisionServer.begin();

  uint32_t start = millis();
  while (true) {
    provisionServer.handleClient();
    delay(5);
    if (PROVISION_PORTAL_TIMEOUT_MS > 0 && millis() - start > PROVISION_PORTAL_TIMEOUT_MS) {
      ESP.restart();
    }
  }
}
