#include <Preferences.h>
#include <WebServer.h>
#include <DNSServer.h>
#include "esp_eap_client.h"
#include "esp_wifi.h"

Preferences nodePrefs;
WebServer provisionServer(80);
DNSServer provisionDnsServer;

String gWifiSsid;
String gWifiPassword;
String gWifiSecurity;
String gWifiIdentity;
String gWifiUsername;
String gBaseUrl;
String gNodeId;
String gKeyId;
String gDeviceSecret;
bool gNodeConfigReady = false;
bool gRecoveryPortalMode = false;

struct PendingEnrollmentState {
  bool active;
  String requestId;
  String pollToken;
  String wifiSsid;
  String wifiPassword;
  String wifiSecurity;
  String wifiIdentity;
  String wifiUsername;
  String baseUrl;
};

PendingEnrollmentState pendingEnrollment = {false, "", "", "", "", "", "", "", ""};
uint32_t pendingEnrollmentLastPollMs = 0;
String pendingEnrollmentStatus = "NOT_STARTED";
String pendingEnrollmentMessage = "Submit setup first";

String normalizeBaseUrl(String value) {
  value.trim();
  while (value.endsWith("/")) value.remove(value.length() - 1);
  if (value.indexOf("://") < 0) value = "https://" + value;
  if (value.startsWith("http://") && value.indexOf(".ts.net") > 0) {
    value = "https://" + value.substring(7);
  }
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
  uint8_t mac[6];
  esp_read_mac(mac, ESP_MAC_WIFI_STA);
  char buf[7];
  snprintf(buf, sizeof(buf), "%02X%02X%02X", mac[3], mac[4], mac[5]);
  return String(buf);
}

String hardwareUid() {
  uint8_t mac[6];
  esp_read_mac(mac, ESP_MAC_WIFI_STA);
  char buf[13];
  snprintf(buf, sizeof(buf), "%02X%02X%02X%02X%02X%02X", mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
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

const String &cfgWifiSecurity() {
  return gWifiSecurity;
}

const String &cfgWifiIdentity() {
  return gWifiIdentity;
}

const String &cfgWifiUsername() {
  return gWifiUsername;
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
  gWifiSecurity = nodePrefs.getString("wifi_sec", DEFAULT_WIFI_SECURITY);
  gWifiIdentity = nodePrefs.getString("wifi_ident", "");
  gWifiUsername = nodePrefs.getString("wifi_user", "");
  gBaseUrl = nodePrefs.getString("base_url", DEFAULT_BASE_URL);
  gNodeId = nodePrefs.getString("node_id", DEFAULT_NODE_ID);
  gKeyId = nodePrefs.getString("key_id", DEFAULT_KEY_ID);
  gDeviceSecret = nodePrefs.getString("secret", DEFAULT_DEVICE_SECRET);
  nodePrefs.end();

  gWifiSsid.trim();
  gWifiSecurity.trim();
  gWifiSecurity.toLowerCase();
  gWifiIdentity.trim();
  gWifiUsername.trim();
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
                    const String &wifiSecurity,
                    const String &wifiIdentity,
                    const String &wifiUsername,
                    const String &baseUrl,
                    const String &nodeId,
                    const String &keyId,
                    const String &deviceSecret) {
  String ssid = wifiSsid;
  String securityMode = wifiSecurity;
  String identity = wifiIdentity;
  String username = wifiUsername;
  String url = normalizeBaseUrl(baseUrl);
  String nid = nodeId;
  String kid = keyId;
  String secret = deviceSecret;
  ssid.trim();
  securityMode.trim();
  securityMode.toLowerCase();
  identity.trim();
  username.trim();
  nid.trim();
  kid.trim();
  secret.trim();

  if (securityMode != "personal" && securityMode != "enterprise" && securityMode != "open") securityMode = "personal";
  if (securityMode == "enterprise" && username.length() == 0) return false;
  if (ssid.length() == 0 || url.length() == 0 || nid.length() == 0 || kid.length() == 0 || secret.length() < 32) {
    return false;
  }

  nodePrefs.begin("batnode", false);
  nodePrefs.putString("wifi_ssid", ssid);
  nodePrefs.putString("wifi_pass", wifiPassword);
  nodePrefs.putString("wifi_sec", securityMode);
  nodePrefs.putString("wifi_ident", identity);
  nodePrefs.putString("wifi_user", username);
  nodePrefs.putString("base_url", url);
  nodePrefs.putString("node_id", nid);
  nodePrefs.putString("key_id", kid);
  nodePrefs.putString("secret", secret);
  nodePrefs.end();

  gWifiSsid = ssid;
  gWifiPassword = wifiPassword;
  gWifiSecurity = securityMode;
  gWifiIdentity = identity;
  gWifiUsername = username;
  gBaseUrl = url;
  gNodeId = nid;
  gKeyId = kid;
  gDeviceSecret = secret;
  gNodeConfigReady = true;
  return true;
}

bool saveWifiConfig(const String &wifiSsid,
                    const String &wifiPassword,
                    const String &wifiSecurity,
                    const String &wifiIdentity,
                    const String &wifiUsername,
                    const String &baseUrl) {
  String ssid = wifiSsid;
  String securityMode = wifiSecurity;
  String identity = wifiIdentity;
  String username = wifiUsername;
  String url = normalizeBaseUrl(baseUrl);
  ssid.trim();
  securityMode.trim();
  securityMode.toLowerCase();
  identity.trim();
  username.trim();

  if (securityMode != "personal" && securityMode != "enterprise" && securityMode != "open") {
    securityMode = "personal";
  }
  if (ssid.length() == 0) return false;
  if (url.length() == 0) return false;
  if (securityMode == "enterprise" && (username.length() == 0 || wifiPassword.length() == 0)) return false;
  if (gNodeId.length() == 0 || gKeyId.length() == 0 || gDeviceSecret.length() < 32) {
    return false;
  }

  nodePrefs.begin("batnode", false);
  nodePrefs.putString("wifi_ssid", ssid);
  nodePrefs.putString("wifi_pass", wifiPassword);
  nodePrefs.putString("wifi_sec", securityMode);
  nodePrefs.putString("wifi_ident", identity);
  nodePrefs.putString("wifi_user", username);
  nodePrefs.putString("base_url", url);
  nodePrefs.end();

  gWifiSsid = ssid;
  gWifiPassword = wifiPassword;
  gWifiSecurity = securityMode;
  gWifiIdentity = identity;
  gWifiUsername = username;
  gBaseUrl = url;
  gNodeConfigReady = true;
  return true;
}

bool beginWiFiConnection(const String &ssid,
                         const String &securityMode,
                         const String &identity,
                         const String &username,
                         const String &password) {
  WiFi.disconnect(false, false);
  delay(50);
  WiFi.setAutoReconnect(true);
  esp_wifi_sta_enterprise_disable();

  if (securityMode == "enterprise") {
    String outerIdentity = identity.length() ? identity : username;
    if (username.length() == 0 || password.length() == 0) return false;
    if (esp_eap_client_set_identity((const unsigned char *)outerIdentity.c_str(), outerIdentity.length()) != ESP_OK) return false;
    if (esp_eap_client_set_username((const unsigned char *)username.c_str(), username.length()) != ESP_OK) return false;
    if (esp_eap_client_set_password((const unsigned char *)password.c_str(), password.length()) != ESP_OK) return false;
    if (esp_wifi_sta_enterprise_enable() != ESP_OK) return false;
    WiFi.begin(ssid.c_str());
  } else if (securityMode == "open") {
    WiFi.begin(ssid.c_str());
  } else {
    WiFi.begin(ssid.c_str(), password.c_str());
  }
  return true;
}

bool provisioningForced() {
  nodePrefs.begin("batnode", false);
  bool requested = nodePrefs.getBool("force_setup", false);
  if (requested) nodePrefs.putBool("force_setup", false);
  nodePrefs.end();
  if (requested) return true;
#if PROVISION_FORCE_PIN >= 0
  pinMode(PROVISION_FORCE_PIN, INPUT_PULLUP);
  delay(5);
  return digitalRead(PROVISION_FORCE_PIN) == LOW;
#else
  return false;
#endif
}

void requestProvisioningOnNextBoot() {
  nodePrefs.begin("batnode", false);
  nodePrefs.putBool("force_setup", true);
  nodePrefs.end();
}

String portalPage(const String &message) {
  String suffix = chipSuffix();
  String html;
  html.reserve(9000);
  html += F("<!doctype html><html><head><meta name='viewport' content='width=device-width,initial-scale=1'>");
  html += F("<title>Bat Node Setup</title><style>");
  html += F("body{font-family:system-ui,-apple-system,Segoe UI,sans-serif;margin:0;background:#f7f7f3;color:#1d1d1b}");
  html += F("main{max-width:760px;margin:auto;padding:24px}section{background:#fff;border:1px solid #ddd;border-radius:8px;padding:18px;margin:14px 0}");
  html += F("label{display:block;font-weight:650;margin-top:12px}input,select{width:100%;box-sizing:border-box;padding:10px;border:1px solid #bbb;border-radius:6px;font-size:16px;background:#fff}");
  html += F("button{margin-top:16px;margin-right:8px;padding:11px 14px;border:0;border-radius:6px;background:#145a7a;color:white;font-weight:700;font-size:16px}");
  html += F("button.secondary{background:#37633f}");
  html += F(".danger{background:#8d2c20}.msg{padding:12px;border-radius:6px;background:#e9f2ef;border:1px solid #b7d4ca}.hint{color:#555;font-size:14px}.hidden{display:none}summary{font-weight:700;cursor:pointer}</style>");
  html += F("<script>function wifiMode(){var s=document.getElementById('wifi_security');document.getElementById('enterprise_fields').className=s.value==='enterprise'?'':'hidden';}</script></head><body><main>");
  html += F("<h1>Bat Node Setup</h1>");
  if (gNodeConfigReady) {
    html += F("<p class='hint'>Update Wi-Fi for existing node <strong>");
    html += htmlEscape(gNodeId);
    html += F("</strong>. Use <strong>Save Wi-Fi only</strong> for the same server, or <strong>Re-enroll with dashboard</strong> after server history was cleared.</p>");
  } else {
    html += F("<p class='hint'>Connect Wi-Fi, submit this node, then approve it from the Bat Node dashboard. Credentials are saved automatically.</p>");
  }
  if (message.length()) {
    html += F("<div class='msg'>");
    html += htmlEscape(message);
    html += F("</div>");
  }

  html += gNodeConfigReady ? F("<section><h2>Change Wi-Fi or re-enroll</h2>") : F("<section><h2>Connect and enroll</h2>");
  html += gNodeConfigReady ? F("<form method='post' action='/wifi'>") : F("<form method='post' action='/enroll'>");
  html += F("<label>Wi-Fi SSID</label><input name='wifi_ssid' value='");
  html += htmlEscape(gWifiSsid);
  html += F("' required>");
  html += F("<label>Wi-Fi security</label><select id='wifi_security' name='wifi_security' onchange='wifiMode()'>");
  html += gWifiSecurity == "enterprise" ? F("<option value='personal'>Personal password</option><option value='enterprise' selected>Enterprise username and password</option><option value='open'>Open network</option>") :
          gWifiSecurity == "open" ? F("<option value='personal'>Personal password</option><option value='enterprise'>Enterprise username and password</option><option value='open' selected>Open network</option>") :
          F("<option value='personal' selected>Personal password</option><option value='enterprise'>Enterprise username and password</option><option value='open'>Open network</option>");
  html += F("</select><div id='enterprise_fields' class='");
  html += gWifiSecurity == "enterprise" ? F("") : F("hidden");
  html += F("'><label>Enterprise identity</label><input name='wifi_identity' value='");
  html += htmlEscape(gWifiIdentity);
  html += F("' placeholder='Usually the same as username'><label>Enterprise username</label><input name='wifi_username' value='");
  html += htmlEscape(gWifiUsername);
  html += F("'></div><label>Wi-Fi password</label><input name='wifi_password' type='password' value='");
  html += htmlEscape(gWifiPassword);
  html += F("'>");
  html += F("<label>Public server URL</label><input name='base_url' value='");
  html += htmlEscape(gBaseUrl.length() ? gBaseUrl : String(DEFAULT_BASE_URL));
  html += F("' placeholder='https://your-computer.tailnet.ts.net' required>");
  html += F("<label>Node name</label><input name='node_name' value='");
  if (gNodeConfigReady) {
    html += htmlEscape(gNodeId);
    html += F("'><input type='hidden' name='preferred_node_id' value='");
    html += htmlEscape(gNodeId);
    html += F("'>");
    html += F("<button type='submit' formaction='/wifi'>Save Wi-Fi only</button>");
    html += F("<button class='secondary' type='submit' formaction='/enroll'>Re-enroll with dashboard</button>");
    html += F("<p class='hint'>Re-enroll keeps this hardware in setup mode until the dashboard approves it and returns fresh credentials.</p></form></section>");
  } else {
    html += F("Bat Node ");
    html += suffix;
    html += F("'>");
    html += F("<button type='submit'>Connect and request approval</button></form></section>");
  }

  html += F("<section><details><summary>Advanced manual recovery</summary><p class='hint'>Use only when restoring credentials without dashboard enrollment.</p>");
  html += F("<form method='post' action='/save'>");
  html += F("<label>Wi-Fi SSID</label><input name='wifi_ssid' value='");
  html += htmlEscape(gWifiSsid);
  html += F("' required>");
  html += F("<label>Wi-Fi security</label><select name='wifi_security'><option value='personal'>Personal password</option><option value='enterprise'>Enterprise username and password</option><option value='open'>Open network</option></select>");
  html += F("<label>Enterprise identity</label><input name='wifi_identity' value='");
  html += htmlEscape(gWifiIdentity);
  html += F("'><label>Enterprise username</label><input name='wifi_username' value='");
  html += htmlEscape(gWifiUsername);
  html += F("'>");
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
  html += F("<button type='submit'>Save manual configuration</button></form></details></section>");

  html += F("<section><h2>Reset</h2><form method='post' action='/clear'>");
  html += F("<button class='danger' type='submit'>Clear Saved Config</button></form></section>");
  html += F("</main></body></html>");
  return html;
}

void sendPortalPage(const String &message) {
  provisionServer.send(200, "text/html", portalPage(message));
}

bool waitForProvisionWiFi(const String &ssid,
                          const String &securityMode,
                          const String &identity,
                          const String &username,
                          const String &password) {
  WiFi.mode(WIFI_AP_STA);
  if (!beginWiFiConnection(ssid, securityMode, identity, username, password)) return false;
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

bool requestServerEnrollment(const String &baseUrl,
                             const String &nodeName,
                             const String &preferredNodeId,
                             String &requestIdOut,
                             String &pollTokenOut,
                             String &errorOut) {
  WiFiClient plainClient;
  WiFiClientSecure secureClient;
  HTTPClient http;
  String url = normalizeBaseUrl(baseUrl) + "/v1/enrollment/request";
  if (!beginHttpClient(http, plainClient, secureClient, url)) {
    errorOut = "Could not open enrollment URL";
    return false;
  }
  http.setTimeout(HTTP_TIMEOUT_MS);
  http.addHeader("Content-Type", "application/json");
  const char *responseHeaders[] = {"Location"};
  http.collectHeaders(responseHeaders, 1);

  StaticJsonDocument<768> doc;
  doc["hardware_uid"] = hardwareUid();
  doc["node_name"] = nodeName;
  if (preferredNodeId.length()) doc["preferred_node_id"] = preferredNodeId;
  doc["hardware_version"] = "ESP32 AudioMoth bridge";
  doc["firmware_version"] = "Moth_Node_ESPBridge enrollment-v2";
  String body;
  serializeJson(doc, body);

  int code = http.POST(body);
  String response = http.getString();
  String redirectLocation = http.header("Location");
  http.end();

  if (code < 200 || code >= 300) {
    errorOut = "Server returned HTTP " + String(code) + ": " + response;
    if (redirectLocation.length()) errorOut += " Redirect: " + redirectLocation;
    Serial.println(errorOut);
    return false;
  }

  StaticJsonDocument<768> resp;
  DeserializationError err = deserializeJson(resp, response);
  if (err || !(resp["ok"] | false)) {
    errorOut = "Could not parse enrollment response";
    return false;
  }

  requestIdOut = String((const char *)(resp["request_id"] | ""));
  pollTokenOut = String((const char *)(resp["poll_token"] | ""));
  if (requestIdOut.length() == 0 || pollTokenOut.length() < 16) {
    errorOut = "Enrollment response was missing request credentials";
    return false;
  }
  return true;
}

bool pollServerEnrollment(String &statusOut,
                          String &nodeIdOut,
                          String &keyIdOut,
                          String &secretOut,
                          String &errorOut) {
  WiFiClient plainClient;
  WiFiClientSecure secureClient;
  HTTPClient http;
  String path = "/v1/enrollment/status/" + pendingEnrollment.requestId;
  String url = pendingEnrollment.baseUrl + path;
  if (!beginHttpClient(http, plainClient, secureClient, url)) {
    errorOut = "Could not open enrollment status URL";
    return false;
  }
  http.setTimeout(HTTP_TIMEOUT_MS);
  http.addHeader("Content-Type", "application/json");

  StaticJsonDocument<256> doc;
  doc["poll_token"] = pendingEnrollment.pollToken;
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
  if (deserializeJson(resp, response) || !(resp["ok"] | false)) {
    errorOut = "Could not parse enrollment status";
    return false;
  }
  statusOut = String((const char *)(resp["status"] | "PENDING"));
  if (statusOut == "APPROVED") {
    nodeIdOut = String((const char *)(resp["node_id"] | ""));
    keyIdOut = String((const char *)(resp["key_id"] | ""));
    secretOut = String((const char *)(resp["device_secret"] | ""));
    if (nodeIdOut.length() == 0 || keyIdOut.length() == 0 || secretOut.length() < 32) {
      errorOut = "Approved enrollment was missing credentials";
      return false;
    }
  }
  return true;
}

String enrollmentPendingPage() {
  String html;
  html.reserve(2600);
  html += F("<!doctype html><html><head><meta name='viewport' content='width=device-width,initial-scale=1'><title>Waiting for approval</title><style>");
  html += F("body{font-family:system-ui,-apple-system,Segoe UI,sans-serif;background:#f7f7f3;color:#1d1d1b;margin:0}main{max-width:620px;margin:auto;padding:30px}section{background:white;border:1px solid #ddd;border-radius:8px;padding:24px}.state{font-size:20px;font-weight:700}.hint{color:#555}</style></head><body><main><section>");
  html += F("<h1>Approval requested</h1><p class='state' id='state'>Waiting for the dashboard...</p>");
  html += F("<p class='hint'>Open the Bat Node dashboard on your computer, choose Add Nodes, and approve this hardware ID:</p><p><strong>");
  html += hardwareUid();
  html += F("</strong></p><p class='hint' id='detail'>This page checks automatically.</p></section></main><script>");
  html += F("async function check(){try{let r=await fetch('/enrollment-status',{cache:'no-store'});let d=await r.json();document.getElementById('state').textContent=d.message||d.status;if(d.status==='APPROVED'){document.getElementById('detail').textContent='Credentials saved. The node is restarting.';return;}if(d.status==='REJECTED'||d.status==='EXPIRED'){document.getElementById('detail').textContent='Return to setup and submit again.';return;}setTimeout(check,");
  html += String(ENROLLMENT_POLL_INTERVAL_MS);
  html += F(");}catch(e){document.getElementById('detail').textContent='Connection interrupted; retrying...';setTimeout(check,5000);}}check();</script></body></html>");
  return html;
}

void handlePortalRoot() {
  sendPortalPage("");
}

void redirectToPortalRoot() {
  String url = "http://" + WiFi.softAPIP().toString() + "/";
  provisionServer.sendHeader("Location", url, true);
  provisionServer.send(302, "text/plain", "Open Bat Node setup: " + url);
}

void handleCaptivePortalProbe() {
  redirectToPortalRoot();
}

void handlePortalNotFound() {
  if (provisionServer.method() == HTTP_GET) {
    redirectToPortalRoot();
    return;
  }
  handlePortalRoot();
}

void handleManualSave() {
  bool ok = saveNodeConfig(
    provisionServer.arg("wifi_ssid"),
    provisionServer.arg("wifi_password"),
    provisionServer.arg("wifi_security"),
    provisionServer.arg("wifi_identity"),
    provisionServer.arg("wifi_username"),
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

void handleWifiSave() {
  String ssid = provisionServer.arg("wifi_ssid");
  String password = provisionServer.arg("wifi_password");
  String securityMode = provisionServer.arg("wifi_security");
  String identity = provisionServer.arg("wifi_identity");
  String username = provisionServer.arg("wifi_username");
  String baseUrl = normalizeBaseUrl(provisionServer.arg("base_url"));
  ssid.trim();
  securityMode.trim();
  securityMode.toLowerCase();
  identity.trim();
  username.trim();

  if (!gNodeConfigReady) {
    sendPortalPage("No saved node identity was found. Use Connect and enroll instead.");
    return;
  }
  if (ssid.length() == 0) {
    sendPortalPage("Enter a Wi-Fi network name.");
    return;
  }
  if (baseUrl.length() == 0) {
    sendPortalPage("Enter the server URL.");
    return;
  }
  if (baseUrl.indexOf(":8443") > 0) {
    sendPortalPage("Use the ESP32 server URL without :8443. Port 8443 is only for the private dashboard.");
    return;
  }
  if (securityMode == "enterprise" && (username.length() == 0 || password.length() == 0)) {
    sendPortalPage("Enterprise Wi-Fi needs both username and password.");
    return;
  }
  if (!waitForProvisionWiFi(ssid, securityMode, identity, username, password)) {
    sendPortalPage("Could not connect to that Wi-Fi network. Check the security mode and credentials, then try again.");
    return;
  }
  if (!saveWifiConfig(ssid, password, securityMode, identity, username, baseUrl)) {
    sendPortalPage("Wi-Fi connected, but the settings could not be saved.");
    return;
  }

  provisionServer.send(200, "text/html", "<html><body><h1>Wi-Fi updated</h1><p>The node identity was preserved. Restarting...</p></body></html>");
  delay(1000);
  ESP.restart();
}

void handleStartEnrollment() {
  String ssid = provisionServer.arg("wifi_ssid");
  String password = provisionServer.arg("wifi_password");
  String securityMode = provisionServer.arg("wifi_security");
  String identity = provisionServer.arg("wifi_identity");
  String username = provisionServer.arg("wifi_username");
  String baseUrl = normalizeBaseUrl(provisionServer.arg("base_url"));
  String nodeName = provisionServer.arg("node_name");
  String preferredNodeId = provisionServer.arg("preferred_node_id");
  ssid.trim();
  securityMode.trim();
  securityMode.toLowerCase();
  identity.trim();
  username.trim();
  preferredNodeId.trim();
  if (nodeName.length() == 0) nodeName = "Bat Node " + chipSuffix();
  if (preferredNodeId.length() == 0 && gNodeConfigReady) preferredNodeId = gNodeId;

  if (ssid.length() == 0 || baseUrl.length() == 0) {
    sendPortalPage("Enrollment needs a Wi-Fi SSID and public server URL.");
    return;
  }
  if (baseUrl.indexOf(":8443") > 0) {
    sendPortalPage("Use the ESP32 server URL without :8443. Port 8443 is only for the private dashboard.");
    return;
  }
  if (securityMode == "enterprise" && (username.length() == 0 || password.length() == 0)) {
    sendPortalPage("Enterprise Wi-Fi needs both username and password.");
    return;
  }

  if (!waitForProvisionWiFi(ssid, securityMode, identity, username, password)) {
    sendPortalPage("Could not connect to field Wi-Fi. Check security mode and credentials, then try again.");
    return;
  }
  if (baseUrl.startsWith("https://") && estimatedEpochUtc() <= 1700000000UL && !syncClockFromNtp()) {
    sendPortalPage("Wi-Fi connected, but internet time was unavailable. Secure enrollment could not verify the server certificate.");
    return;
  }

  String requestId;
  String pollToken;
  String error;
  if (!requestServerEnrollment(baseUrl, nodeName, preferredNodeId, requestId, pollToken, error)) {
    sendPortalPage(error);
    return;
  }

  pendingEnrollment.active = true;
  pendingEnrollment.requestId = requestId;
  pendingEnrollment.pollToken = pollToken;
  pendingEnrollment.wifiSsid = ssid;
  pendingEnrollment.wifiPassword = password;
  pendingEnrollment.wifiSecurity = securityMode;
  pendingEnrollment.wifiIdentity = identity;
  pendingEnrollment.wifiUsername = username;
  pendingEnrollment.baseUrl = baseUrl;
  pendingEnrollmentStatus = "PENDING";
  pendingEnrollmentMessage = "Waiting for dashboard approval";
  pendingEnrollmentLastPollMs = millis() - ENROLLMENT_POLL_INTERVAL_MS;
  provisionServer.send(200, "text/html", enrollmentPendingPage());
}

void processPendingEnrollment() {
  if (!pendingEnrollment.active) return;
  if (pendingEnrollmentStatus == "REJECTED" || pendingEnrollmentStatus == "EXPIRED" || pendingEnrollmentStatus == "SAVE_FAILED") return;
  if (WiFi.status() != WL_CONNECTED) {
    pendingEnrollmentStatus = "RETRYING";
    pendingEnrollmentMessage = "Field Wi-Fi disconnected; waiting to reconnect";
    return;
  }

  String status;
  String nodeId;
  String keyId;
  String secret;
  String error;
  if (!pollServerEnrollment(status, nodeId, keyId, secret, error)) {
    pendingEnrollmentStatus = "RETRYING";
    pendingEnrollmentMessage = error;
    Serial.println("Enrollment poll failed: " + error);
    return;
  }

  pendingEnrollmentStatus = status;
  pendingEnrollmentMessage = status == "PENDING" ? "Waiting for dashboard approval" : status;
  if (status == "APPROVED") {
    bool saved = saveNodeConfig(
      pendingEnrollment.wifiSsid,
      pendingEnrollment.wifiPassword,
      pendingEnrollment.wifiSecurity,
      pendingEnrollment.wifiIdentity,
      pendingEnrollment.wifiUsername,
      pendingEnrollment.baseUrl,
      nodeId,
      keyId,
      secret
    );
    if (!saved) {
      pendingEnrollmentStatus = "SAVE_FAILED";
      pendingEnrollmentMessage = "Credentials arrived but could not be saved";
      Serial.println(pendingEnrollmentMessage);
      return;
    }
    Serial.printf("Enrollment approved as %s; credentials saved. Restarting.\n", nodeId.c_str());
    delay(1200);
    ESP.restart();
  }
}

void handleEnrollmentStatus() {
  StaticJsonDocument<384> doc;
  doc["ok"] = pendingEnrollment.active;
  doc["status"] = pendingEnrollmentStatus;
  doc["message"] = pendingEnrollmentMessage;
  String body;
  serializeJson(doc, body);
  provisionServer.send(pendingEnrollment.active ? 200 : 409, "application/json", body);
}

void handleClearConfig() {
  nodePrefs.begin("batnode", false);
  nodePrefs.clear();
  nodePrefs.end();
  provisionServer.send(200, "text/html", "<html><body><h1>Cleared</h1><p>Restarting setup portal...</p></body></html>");
  delay(1000);
  ESP.restart();
}

void runProvisioningPortal(bool recoveryMode) {
  gRecoveryPortalMode = recoveryMode;
  String apSsid = String(PROVISION_AP_PREFIX) + "-" + chipSuffix();
  WiFi.mode(WIFI_AP);
  bool apOk = WiFi.softAP(apSsid.c_str(), PROVISION_AP_PASSWORD);

  Serial.println();
  Serial.println("=== Bat Node setup portal ===");
  Serial.printf("Mode: %s\n", recoveryMode ? "Wi-Fi recovery" : "first-time setup");
  Serial.printf("AP: %s\n", apSsid.c_str());
  Serial.printf("Password: %s\n", PROVISION_AP_PASSWORD);
  Serial.print("Setup URL: http://");
  Serial.println(WiFi.softAPIP());
  if (!apOk) Serial.println("Warning: setup AP did not report success");

  provisionDnsServer.start(53, "*", WiFi.softAPIP());

  provisionServer.on("/", HTTP_GET, handlePortalRoot);
  provisionServer.on("/generate_204", HTTP_GET, handleCaptivePortalProbe);
  provisionServer.on("/gen_204", HTTP_GET, handleCaptivePortalProbe);
  provisionServer.on("/hotspot-detect.html", HTTP_GET, handleCaptivePortalProbe);
  provisionServer.on("/library/test/success.html", HTTP_GET, handleCaptivePortalProbe);
  provisionServer.on("/connecttest.txt", HTTP_GET, handleCaptivePortalProbe);
  provisionServer.on("/ncsi.txt", HTTP_GET, handleCaptivePortalProbe);
  provisionServer.on("/fwlink", HTTP_GET, handleCaptivePortalProbe);
  provisionServer.on("/wifi", HTTP_POST, handleWifiSave);
  provisionServer.on("/save", HTTP_POST, handleManualSave);
  provisionServer.on("/enroll", HTTP_POST, handleStartEnrollment);
  provisionServer.on("/enrollment-status", HTTP_GET, handleEnrollmentStatus);
  provisionServer.on("/clear", HTTP_POST, handleClearConfig);
  provisionServer.onNotFound(handlePortalNotFound);
  provisionServer.begin();

  uint32_t start = millis();
  uint32_t timeoutMs = recoveryMode ? WIFI_RECOVERY_PORTAL_TIMEOUT_MS : PROVISION_PORTAL_TIMEOUT_MS;
  while (true) {
    provisionDnsServer.processNextRequest();
    provisionServer.handleClient();
    if (pendingEnrollment.active && millis() - pendingEnrollmentLastPollMs >= ENROLLMENT_POLL_INTERVAL_MS) {
      pendingEnrollmentLastPollMs = millis();
      processPendingEnrollment();
    }
    delay(5);
    if (timeoutMs > 0 && millis() - start > timeoutMs) {
      Serial.println("Wi-Fi recovery portal timed out; returning to the sleep schedule.");
      provisionServer.stop();
      provisionDnsServer.stop();
      WiFi.softAPdisconnect(true);
      return;
    }
  }
}
