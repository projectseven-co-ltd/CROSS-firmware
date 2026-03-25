/*
 * CROSS WiFi Onboarding
 * Minimal firmware focused on WiFi provisioning via AP + captive portal/API.
 * Now uses periodic NocoDB polling for prayer events.
 */

// LED Strip Configuration - Choose ONE:
#define LED_TYPE_COMMON_ANODE    // Non-addressable RGB strip (3 pins: R, G, B)
// #define LED_TYPE_ADDRESSABLE  // WS2812/SK6812 addressable strip (1 data pin)

#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <ESPmDNS.h>
#include <Preferences.h>
#include <WebServer.h>
#include <ArduinoJson.h>
#include <DNSServer.h>
#include <HTTPClient.h>
#include <HTTPUpdate.h>

#ifdef LED_TYPE_ADDRESSABLE
#include <FastLED.h>
#endif

const char* FIRMWARE_VERSION = "4.0";
const char* GITHUB_API_URL = "https://api.github.com/repos/projectseven-co-ltd/CROSS-firmware/releases/latest";

// LED Strip Configuration
#ifdef LED_TYPE_COMMON_ANODE
  // Non-addressable RGB strip (common anode)
  #define LED_PIN_R 16       // GPIO pin for Red channel
  #define LED_PIN_G 17       // GPIO pin for Green channel
  #define LED_PIN_B 18       // GPIO pin for Blue channel
  #define PWM_CHANNEL_R 0    // PWM channel for Red
  #define PWM_CHANNEL_G 1    // PWM channel for Green
  #define PWM_CHANNEL_B 2    // PWM channel for Blue
  #define PWM_FREQ 5000      // PWM frequency
  #define PWM_RESOLUTION 8   // 8-bit resolution (0-255)
#endif

#ifdef LED_TYPE_ADDRESSABLE
  // Addressable LED strip (WS2812/SK6812)
  #define LED_DATA_PIN 16    // GPIO pin for data
  #define NUM_LEDS 1         // Number of LEDs in strip
  #define LED_TYPE_CHIP WS2812B
  #define COLOR_ORDER GRB
  CRGB leds[NUM_LEDS];
#endif

// SchedKit Alerts API — cross polls this instead of NocoDB prayers
const char* SCHEDKIT_BASE_URL  = "https://schedkit.net";
const char* SCHEDKIT_API_KEY   = "p7s_pJSiN-U9RxUQtxYauUFYOHuj7f6UwLJE"; // x-api-key
const char* NOCODB_BASE_URL    = "https://noco.app.p7n.net";

const char* NOCODB_CROSSES_PATH = "/api/v1/db/data/noco/pdrfbzgtno2cf9l/mqbvkiidtv2xl99";
const char* NOCODB_API_TOKEN = "fDhJb1s9aK8yQsj99iSt6DOe9o518yGAwAdwezn1";
const uint32_t ALERT_POLL_MS  = 30000; // poll alerts every 30s

const char* BIBLICAL_NAMES[] = {
  "luke", "john", "mark", "matthew", "paul", "peter", "james", "timothy",
  "david", "daniel", "samuel", "joseph", "moses", "joshua", "elijah", "isaac",
  "jacob", "noah", "abraham", "solomon", "adam", "ezra", "jonah", "micah"
};
const int BIBLICAL_NAMES_COUNT = 24;

String DEVICE_NAME;
String HOSTNAME;      // Display name (e.g., luke3a7.cross)
String MDNS_HOSTNAME; // Actual mDNS name (e.g., luke3a7.local)
String AP_SSID;
String DEVICE_KEY;    // Stable unique identifier derived from chip MAC
Preferences preferences;
WebServer server(80);
DNSServer dnsServer;

const byte DNS_PORT = 53;

const IPAddress AP_IP(192, 168, 4, 1);
const IPAddress AP_GATEWAY(192, 168, 4, 1);
const IPAddress AP_SUBNET(255, 255, 255, 0);

bool apModeActive = false;
bool serverStarted = false;
bool isConnected = false;

// Serial command buffer for local maintenance commands
String serialBuffer;

String crossId;
String apiToken;  // Optional API authentication token
String registeredEmail;  // User's email for cross registration

// NocoDB poll state
unsigned long lastAlertPollMs = 0;


long intercessionCount = 0;
long heartbeatCount = 0;  // Activity level counter (increments on each heartbeat)
bool versionSyncPending = false; // Tracks if firmware version needs DB sync
String versionSyncVersion;

// OTA update state
unsigned long lastOTACheckMs = 0;
const uint32_t OTA_CHECK_INTERVAL_MS = 86400000; // 24 hours in milliseconds

// Email sync state
bool emailUpdateFlag = false;  // Flag to force email pull from database on boot

// In-memory log ring buffer exposed via /api/logs
const size_t LOG_CAPACITY = 150;
String logBuffer[LOG_CAPACITY];
size_t logHead = 0;
size_t logCount = 0;

void appendLog(const String &line) {
  logBuffer[logHead] = line;
  logHead = (logHead + 1) % LOG_CAPACITY;
  if (logCount < LOG_CAPACITY) {
    logCount++;
  }
}

void logLine(const String &line) {
  Serial.println(line);
  appendLog(line);
}

String buildLogDump() {
  if (logCount == 0) return String("no logs yet");

  String dump;
  dump.reserve(logCount * 48);
  size_t startIndex = (logHead + LOG_CAPACITY - logCount) % LOG_CAPACITY;
  for (size_t i = 0; i < logCount; i++) {
    size_t idx = (startIndex + i) % LOG_CAPACITY;
    dump += logBuffer[idx];
    if (i < logCount - 1) dump += "\n";
  }
  return dump;
}

// Forward declarations
bool connectToSavedWiFi();
void startAPMode();
void loadDeviceConfig();
String computeDeviceKey();
String macSuffix();
void handlePortal();
void handlePortalSave();
void handleStatusAPI();
void handleScanWiFiAPI();
void handleConfigureWiFiAPI();
void handleWiFiReset();
void handleSetName();
void handleSetToken();
void handleSetEmail();
void handleReboot();
void handleLogsAPI();
void handleOTAUpdate();
bool isAuthorized();
void checkForUpdate();
void startHttpServer(bool inAPMode);
void startmDNS();
void runPrayerPulse();
void runAPModePulse();
void pollAlertsIfDue();
void pollAlerts();
void processSerialInput();
void handleSerialCommand(const String &line);
void registerCrossIfNeeded();
bool fetchExistingCross();
bool createCrossRecord();
bool updateCrossVersionInNocoDB();
bool updateCrossEmailInNocoDB();
bool fetchEmailFromNocoDB();
void attemptVersionSyncIfPending();
bool verifyCrossIdExists();

// ============================================================================
// SETUP & LOOP
// ============================================================================

// ============================================================================
// LED ABSTRACTION LAYER
// ============================================================================

void initLEDs() {
#ifdef LED_TYPE_COMMON_ANODE
  ledcAttach(LED_PIN_R, PWM_FREQ, PWM_RESOLUTION);
  ledcAttach(LED_PIN_G, PWM_FREQ, PWM_RESOLUTION);
  ledcAttach(LED_PIN_B, PWM_FREQ, PWM_RESOLUTION);
  ledcWrite(LED_PIN_R, 255);
  ledcWrite(LED_PIN_G, 255);
  ledcWrite(LED_PIN_B, 255);
#endif
#ifdef LED_TYPE_ADDRESSABLE
  FastLED.addLeds<LED_TYPE_CHIP, LED_DATA_PIN, COLOR_ORDER>(leds, NUM_LEDS);
  FastLED.clear();
  FastLED.show();
#endif
}

void setLEDColor(uint8_t r, uint8_t g, uint8_t b) {
#ifdef LED_TYPE_COMMON_ANODE
  // Common anode: invert values (255 = off, 0 = on)
  ledcWrite(LED_PIN_R, 255 - r);
  ledcWrite(LED_PIN_G, 255 - g);
  ledcWrite(LED_PIN_B, 255 - b);
#endif
#ifdef LED_TYPE_ADDRESSABLE
  leds[0] = CRGB(r, g, b);
  FastLED.show();
#endif
}

void clearLEDs() {
#ifdef LED_TYPE_COMMON_ANODE
  ledcWrite(LED_PIN_R, 255);
  ledcWrite(LED_PIN_G, 255);
  ledcWrite(LED_PIN_B, 255);
#endif
#ifdef LED_TYPE_ADDRESSABLE
  FastLED.clear();
  FastLED.show();
#endif
}

// ============================================================================
// SETUP & LOOP
// ============================================================================

void setup() {
  Serial.begin(115200);
  delay(100);

  // Initialize LED strip
  initLEDs();
  // Subtle boot pulse to indicate power-on
  runBootPulse();

  logLine("CROSS WiFi Onboarding v" + String(FIRMWARE_VERSION));
  preferences.begin("cross", false);
  loadDeviceConfig();

  crossId = preferences.getString("cross_id", "");
  apiToken = preferences.getString("api_token", "");
  registeredEmail = preferences.getString("reg_email", "");
  logLine("[Boot] reg_email='" + registeredEmail + "'");
  // restore last seen prayer timestamp
  
  
  intercessionCount = preferences.getLong("intercession_count", 0);
  heartbeatCount = preferences.getLong("heartbeat_count", 0);
  versionSyncPending = preferences.getBool("version_sync_pending", false);
  versionSyncVersion = preferences.getString("version_sync_version", "");
  emailUpdateFlag = preferences.getBool("email_update_flag", false);
  logLine("[Boot] emailUpdateFlag=" + String(emailUpdateFlag ? "true" : "false"));

  if (!connectToSavedWiFi()) {
    startAPMode();
  } else {
    startmDNS();
    startHttpServer(false);
    syncTimeWithNTP();
    registerCrossIfNeeded();
    attemptVersionSyncIfPending();
    // Ensure NocoDB reflects current firmware on boot
    updateCrossVersionInNocoDB();
    
    // ALWAYS sync email to NocoDB if we have one stored, regardless of registration state
    if (registeredEmail.length() > 0) {
      logLine("[Email] Syncing email to NocoDB: " + registeredEmail);
      updateCrossEmailInNocoDB();
    } else {
      logLine("[Email] No email stored locally");
    }
    
    // Handle email update flag if set (pull from DB)
    if (emailUpdateFlag) {
      logLine("[Email] Email update flag set, pulling from database...");
      if (fetchEmailFromNocoDB()) {
        logLine("[Email] Email pulled from database successfully");
        emailUpdateFlag = false;
        preferences.putBool("email_update_flag", false);
        // Sync the pulled email back to ensure consistency
        if (registeredEmail.length() > 0) {
          updateCrossEmailInNocoDB();
        }
      } else {
        logLine("[Email] Failed to pull email from database");
      }
    }
    
    // Check for OTA update on boot
    checkForUpdate();
  }

  logLine("READY");
}

void loop() {
  if (apModeActive) {
    dnsServer.processNextRequest();
    // Pulse yellow every 5 seconds to indicate AP mode
    static unsigned long lastAPPulse = 0;
    if (millis() - lastAPPulse > 5000) {
      lastAPPulse = millis();
      runAPModePulse();
    }
  }
  if (serverStarted) {
    server.handleClient();
  }
  if (!apModeActive && isConnected) {
    pollAlertsIfDue();
    checkForOTAUpdateIfDue();
  }
  processSerialInput();
  delay(1);
}

// ============================================================================
// NTP TIME SYNC
// ============================================================================

void syncTimeWithNTP() {
  logLine("Syncing time with NTP...");
  configTime(0, 0, "pool.ntp.org", "time.nist.gov");
  
  time_t now = time(nullptr);
  int attempts = 0;
  while (now < 24 * 3600 && attempts < 20) {
    delay(500);
    Serial.print(".");
    now = time(nullptr);
    attempts++;
  }
  logLine("");
  
  if (now > 24 * 3600) {
    logLine("Time synced: " + String(ctime(&now)));
  } else {
    logLine("NTP sync failed, time may be incorrect");
  }
}

// ============================================================================
// CONFIG
// ============================================================================

void loadDeviceConfig() {
  DEVICE_NAME = preferences.getString("device_name", "");
  if (DEVICE_NAME == "") {
    randomSeed(ESP.getEfuseMac());
    String name = String(BIBLICAL_NAMES[random(BIBLICAL_NAMES_COUNT)]);
    // Always include MAC suffix so hostnames stay unique across units
    DEVICE_NAME = name + "-" + macSuffix();
    preferences.putString("device_name", DEVICE_NAME);
  }

  DEVICE_KEY = preferences.getString("device_key", "");
  if (DEVICE_KEY == "") {
    DEVICE_KEY = computeDeviceKey();
    preferences.putString("device_key", DEVICE_KEY);
  }

  AP_SSID = DEVICE_NAME;
  HOSTNAME = DEVICE_NAME + ".cross";
  MDNS_HOSTNAME = DEVICE_NAME + ".local";

  logLine("CFG name=" + DEVICE_NAME + " mdns=" + MDNS_HOSTNAME + " ap_ssid=" + AP_SSID);
}

String computeDeviceKey() {
  uint64_t mac = ESP.getEfuseMac();
  char buf[17];
  snprintf(buf, sizeof(buf), "%012llX", (unsigned long long)mac);
  return String(buf);
}

String macSuffix() {
  uint64_t mac = ESP.getEfuseMac();
  uint32_t suffix = (uint32_t)((mac >> 24) & 0xFFFFFF); // first 3 bytes (OUI)
  char buf[7];
  snprintf(buf, sizeof(buf), "%06X", (unsigned long)suffix);
  return String(buf);
}

// ============================================================================
// WIFI
// ============================================================================

bool connectToSavedWiFi() {
  String ssid = preferences.getString("wifi_ssid", "");
  if (ssid == "") {
    logLine("[WiFi] No saved credentials");
    return false;
  }

  String pwd = preferences.getString("wifi_password", "");
  logLine("[WiFi] Connecting to: " + ssid);

  WiFi.mode(WIFI_STA);
  
  // Try up to 3 times with delays between attempts
  for (int attempt = 0; attempt < 3; attempt++) {
    WiFi.begin(ssid.c_str(), pwd.c_str());
    
    // Wait up to 20 seconds per attempt (40 x 500ms)
    int connect_attempts = 0;
    while (WiFi.status() != WL_CONNECTED && connect_attempts < 40) {
      delay(500);
      Serial.print(".");
      connect_attempts++;
    }

    if (WiFi.status() == WL_CONNECTED) {
      logLine("[WiFi] Connected! IP: " + WiFi.localIP().toString());
      apModeActive = false;
      isConnected = true;
      return true;
    }
    
    logLine("[WiFi] Attempt " + String(attempt + 1) + " failed, retrying...");
    delay(2000); // Wait 2 seconds before next attempt
  }

  logLine("[WiFi] Failed after 3 attempts, starting AP mode");
  return false;
}

void startAPMode() {
  logLine("AP start ssid=" + AP_SSID);
  WiFi.mode(WIFI_AP);
  WiFi.softAPConfig(AP_IP, AP_GATEWAY, AP_SUBNET);
  WiFi.softAP(AP_SSID.c_str());
  dnsServer.start(DNS_PORT, "*", AP_IP);

  apModeActive = true;

  MDNS.begin(DEVICE_NAME.c_str());
  MDNS.addService("http", "tcp", 80);

  startHttpServer(true);

  logLine("AP ip=192.168.4.1 ssid=" + AP_SSID);
  logLine("Portal http://192.168.4.1/ or http://" + MDNS_HOSTNAME + "/");
  
  // Indicate AP mode with yellow pulse
  runAPModePulse();
}

// ============================================================================
// LED INDICATORS
// ============================================================================

void runAPModePulse() {
  // AP mode indicator: slow, dark purple pulse
  const uint8_t pulseSpeed = 50;   // slower fade speed
  const uint8_t maxBrightness = 40; // darker purple cap
  
  // Dark purple RGB values (low R+B, no G)
  // Fade in
  for (uint8_t brightness = 0; brightness <= maxBrightness; brightness += 5) {
    uint8_t scaledValue = map(brightness, 0, maxBrightness, 0, 255);
    setLEDColor(scaledValue, 0, scaledValue);
    delay(pulseSpeed);
  }
  
  // Hold bright briefly
  delay(400);
  
  // Fade out
  for (uint8_t brightness = maxBrightness; brightness > 0; brightness -= 5) {
    uint8_t scaledValue = map(brightness, 0, maxBrightness, 0, 255);
    setLEDColor(scaledValue, 0, scaledValue);
    delay(pulseSpeed);
  }
  
  // Turn off
  clearLEDs();
}

void runPrayerPulse() {
  // Prayer pulse effect: fade in cyan, hold, fade out
  // Cyan color represents hope and intercession
  const uint8_t pulseSpeed = 20;  // milliseconds per brightness step
  const uint8_t maxBrightness = 100;
  
  // Cyan RGB values (0, 255, 255)
  // Fade in
  for (uint8_t brightness = 0; brightness <= maxBrightness; brightness += 5) {
    uint8_t scaledValue = map(brightness, 0, maxBrightness, 0, 255);
    setLEDColor(0, scaledValue, scaledValue);
    delay(pulseSpeed);
  }
  
  // Hold bright
  delay(500);
  
  // Fade out
  for (uint8_t brightness = maxBrightness; brightness > 0; brightness -= 5) {
    uint8_t scaledValue = map(brightness, 0, maxBrightness, 0, 255);
    setLEDColor(0, scaledValue, scaledValue);
    delay(pulseSpeed);
  }
  
  // Turn off
  clearLEDs();
  
  logLine("[LED] Prayer pulse complete");
}

void attemptVersionSyncIfPending() {
  if (!versionSyncPending && versionSyncVersion == FIRMWARE_VERSION) return;
  if (WiFi.status() != WL_CONNECTED) return;
  if (crossId.length() == 0 || crossId == "null") return;

  const int maxAttempts = 3;
  for (int attempt = 0; attempt < maxAttempts; attempt++) {
    if (updateCrossVersionInNocoDB()) {
      logLine("[NocoDB] Version sync cleared");
      return;
    }
    delay(500);
  }
  logLine("[NocoDB] Version sync still pending after retries");
}

void runBootPulse() {
  // Subtle boot pulse: brief low-intensity teal glow
  const uint8_t pulseSpeed = 25;
  const uint8_t maxBrightness = 50;

  // Teal (mix of green/blue, low intensity)
  for (uint8_t brightness = 0; brightness <= maxBrightness; brightness += 5) {
    uint8_t scaledValue = map(brightness, 0, maxBrightness, 0, 180);
    setLEDColor(0, scaledValue, scaledValue);
    delay(pulseSpeed);
  }

  delay(200);

  for (uint8_t brightness = maxBrightness; brightness > 0; brightness -= 5) {
    uint8_t scaledValue = map(brightness, 0, maxBrightness, 0, 180);
    setLEDColor(0, scaledValue, scaledValue);
    delay(pulseSpeed);
  }

  clearLEDs();
}

// ============================================================================
// NOCODB REGISTRATION (Cross record)
// ============================================================================

void registerCrossIfNeeded() {
  if (crossId.length() > 0 && crossId != "null") {
    if (verifyCrossIdExists()) {
      logLine("[NocoDB] Cross already registered id=" + crossId);
      // Sync email if we have one stored locally
      if (registeredEmail.length() > 0) {
        logLine("[NocoDB] Syncing email for existing cross");
        updateCrossEmailInNocoDB();
      }
      return;
    }
    // Existing id not found on server; clear and re-register
    logLine("[NocoDB] Stored cross id missing on server, re-registering");
    crossId = "";
    preferences.putString("cross_id", "");
  }

  // Clear bad null values
  if (crossId == "null") {
    crossId = "";
    preferences.putString("cross_id", "");
  }

  if (fetchExistingCross()) {
    // After fetching existing cross, sync email if we have one locally that differs
    if (registeredEmail.length() > 0) {
      logLine("[NocoDB] Syncing email after fetching existing cross");
      updateCrossEmailInNocoDB();
    }
    return;
  }

  if (createCrossRecord()) {
    // After creating new cross, sync email if we have one locally
    if (registeredEmail.length() > 0) {
      logLine("[NocoDB] Syncing email after creating new cross");
      updateCrossEmailInNocoDB();
    }
  }
}

bool fetchExistingCross() {
  if (WiFi.status() != WL_CONNECTED) return false;

  String url = String(NOCODB_BASE_URL) + String(NOCODB_CROSSES_PATH);
  url += "?where=(device_key,eq," + DEVICE_KEY + ")";

  logLine("[NocoDB] Lookup URL: " + url);

  HTTPClient http;
  WiFiClientSecure client;
  client.setInsecure(); // TODO: add root CA for production
  http.begin(client, url);
  if (String(NOCODB_API_TOKEN).length() > 0) {
    http.addHeader("xc-token", NOCODB_API_TOKEN);
  }

  int code = http.GET();
  logLine("[NocoDB] Lookup response code: " + String(code));
  
  if (code != 200) {
    logLine("[NocoDB] Cross lookup failed code=" + String(code));
    String response = http.getString();
    logLine("[NocoDB] Response: " + response);
    http.end();
    return false;
  }

  String response = http.getString();
  logLine("[NocoDB] Lookup response: " + response.substring(0, 200));

  DynamicJsonDocument doc(2048);
  DeserializationError err = deserializeJson(doc, response);
  http.end();
  if (err) {
    logLine("[NocoDB] Cross lookup JSON parse error: " + String(err.c_str()));
    return false;
  }

  JsonArray records = doc["list"];
  if (records.isNull()) {
    records = doc["data"].as<JsonArray>();
  }

  // Fallback lookup by device_name if device_key not present on server
  if (records.isNull() || records.size() == 0) {
    String urlByName = String(NOCODB_BASE_URL) + String(NOCODB_CROSSES_PATH) + "?where=(device_name,eq," + DEVICE_NAME + ")";
    logLine("[NocoDB] Fallback lookup by name: " + urlByName);
    HTTPClient http2;
    WiFiClientSecure client2;
    client2.setInsecure();
    http2.begin(client2, urlByName);
    if (String(NOCODB_API_TOKEN).length() > 0) {
      http2.addHeader("xc-token", NOCODB_API_TOKEN);
    }
    int code2 = http2.GET();
    if (code2 == 200) {
      DynamicJsonDocument doc2(2048);
      DeserializationError err2 = deserializeJson(doc2, http2.getString());
      if (!err2) {
        records = doc2["list"];
        if (records.isNull()) {
          records = doc2["data"].as<JsonArray>();
        }
      }
    }
    http2.end();
  }

  if (records.isNull() || records.size() == 0) {
    logLine("[NocoDB] Cross not found, will create");
    return false;
  }

  JsonObject rec = records[0];
  String id = rec["Id"].as<String>();
  if (id.length() == 0) {
    logLine("[NocoDB] Cross lookup missing Id");
    return false;
  }

  crossId = id;
  preferences.putString("cross_id", crossId);
  if (!rec["intercession_count"].isNull()) {
    intercessionCount = rec["intercession_count"].as<long>();
    preferences.putLong("intercession_count", intercessionCount);
  }
  if (!rec["registered_email"].isNull()) {
    String email = rec["registered_email"].as<String>();
    if (email.length() > 0) {
      registeredEmail = email;
      preferences.putString("reg_email", registeredEmail);
    }
  }
  logLine("[NocoDB] Cross found id=" + crossId);
  return true;
}

bool updateCrossVersionInNocoDB() {
  if (WiFi.status() != WL_CONNECTED) return false;
  if (crossId.length() == 0 || crossId == "null") return false;

  String url = String(NOCODB_BASE_URL) + String(NOCODB_CROSSES_PATH) + "?where=(Id,eq," + crossId + ")";

  StaticJsonDocument<256> doc;
  doc["Id"] = crossId;
  doc["firmware_version"] = FIRMWARE_VERSION;

  String payload;
  serializeJson(doc, payload);
  logLine("[NocoDB] Update version payload: " + payload);

  HTTPClient http;
  WiFiClientSecure client;
  client.setInsecure();
  http.begin(client, url);
  http.addHeader("Content-Type", "application/json");
  if (String(NOCODB_API_TOKEN).length() > 0) {
    http.addHeader("xc-token", NOCODB_API_TOKEN);
  }

  int code = http.PATCH(payload);
  logLine("[NocoDB] Update version response code: " + String(code));

  bool success = (code >= 200 && code < 300);
  if (!success) {
    logLine("[NocoDB] Version update failed code=" + String(code));
    String response = http.getString();
    logLine("[NocoDB] Error: " + response);
  } else {
    logLine("[NocoDB] Version updated successfully");
    versionSyncPending = false;
    preferences.putBool("version_sync_pending", false);
    preferences.putString("version_sync_version", FIRMWARE_VERSION);
  }
  http.end();
  return success;
}

void updateCrossIntercessionCount() {
  if (WiFi.status() != WL_CONNECTED) return;
  if (crossId.length() == 0 || crossId == "null") return;

  String url = String(NOCODB_BASE_URL) + String(NOCODB_CROSSES_PATH) + "?where=(Id,eq," + crossId + ")";

  StaticJsonDocument<256> doc;
  doc["Id"] = crossId;
  doc["intercession_count"] = intercessionCount;

  String payload;
  serializeJson(doc, payload);

  HTTPClient http;
  WiFiClientSecure client;
  client.setInsecure();
  http.begin(client, url);
  http.addHeader("Content-Type", "application/json");
  if (String(NOCODB_API_TOKEN).length() > 0) {
    http.addHeader("xc-token", NOCODB_API_TOKEN);
  }

  int code = http.PATCH(payload);
  if (code < 200 || code >= 300) {
    logLine("[NocoDB] Intercession update failed code=" + String(code));
  } else {
    logLine("[NocoDB] Intercession count updated to " + String(intercessionCount));
  }
  http.end();
}

void updateCrossHeartbeat() {
  if (WiFi.status() != WL_CONNECTED) return;
  if (crossId.length() == 0 || crossId == "null") return;

  // Increment heartbeat counter for activity level
  heartbeatCount++;
  preferences.putLong("heartbeat_count", heartbeatCount);

  String url = String(NOCODB_BASE_URL) + String(NOCODB_CROSSES_PATH) + "?where=(Id,eq," + crossId + ")";

  // Use RFC3339 format timestamp for compatibility
  time_t now = time(nullptr);
  struct tm* timeinfo = gmtime(&now);
  char timestamp[30];
  strftime(timestamp, sizeof(timestamp), "%Y-%m-%dT%H:%M:%SZ", timeinfo);

  StaticJsonDocument<512> doc;
  doc["Id"] = crossId;
  doc["last_heartbeat"] = String(timestamp);
  doc["heartbeat_count"] = heartbeatCount;  // Sync activity level to NocoDB

  String payload;
  serializeJson(doc, payload);

  HTTPClient http;
  WiFiClientSecure client;
  client.setInsecure();
  http.begin(client, url);
  http.addHeader("Content-Type", "application/json");
  if (String(NOCODB_API_TOKEN).length() > 0) {
    http.addHeader("xc-token", NOCODB_API_TOKEN);
  }

  int code = http.PATCH(payload);
  if (code >= 200 && code < 300) {
    logLine("[NocoDB] Heartbeat updated count=" + String(heartbeatCount));
  } else {
    logLine("[NocoDB] Heartbeat update failed code=" + String(code));
  }
  http.end();
}

bool updateCrossEmailInNocoDB() {
  if (WiFi.status() != WL_CONNECTED) return false;
  if (crossId.length() == 0 || crossId == "null") return false;
  if (registeredEmail.length() == 0) return false;

  String url = String(NOCODB_BASE_URL) + String(NOCODB_CROSSES_PATH) + "?where=(Id,eq," + crossId + ")";

  StaticJsonDocument<256> doc;
  doc["Id"] = crossId;
  doc["registered_email"] = registeredEmail;

  String payload;
  serializeJson(doc, payload);
  logLine("[NocoDB] Update email payload: " + payload);

  HTTPClient http;
  WiFiClientSecure client;
  client.setInsecure();
  http.begin(client, url);
  http.addHeader("Content-Type", "application/json");
  if (String(NOCODB_API_TOKEN).length() > 0) {
    http.addHeader("xc-token", NOCODB_API_TOKEN);
  }

  int code = http.PATCH(payload);
  logLine("[NocoDB] Update email response code: " + String(code));

  bool success = (code >= 200 && code < 300);
  if (!success) {
    logLine("[NocoDB] Email update failed code=" + String(code));
    String response = http.getString();
    logLine("[NocoDB] Error: " + response);
  } else {
    logLine("[NocoDB] Email synced successfully");
  }
  http.end();
  return success;
}

bool fetchEmailFromNocoDB() {
  if (WiFi.status() != WL_CONNECTED) return false;
  if (crossId.length() == 0 || crossId == "null") return false;

  String url = String(NOCODB_BASE_URL) + String(NOCODB_CROSSES_PATH) + "?where=(Id,eq," + crossId + ")";

  logLine("[Email] Fetching email from NocoDB...");

  HTTPClient http;
  WiFiClientSecure client;
  client.setInsecure();
  http.begin(client, url);
  if (String(NOCODB_API_TOKEN).length() > 0) {
    http.addHeader("xc-token", NOCODB_API_TOKEN);
  }

  int code = http.GET();
  if (code != 200) {
    logLine("[Email] Fetch failed code=" + String(code));
    http.end();
    return false;
  }

  DynamicJsonDocument doc(2048);
  DeserializationError err = deserializeJson(doc, http.getString());
  http.end();
  if (err) {
    logLine("[Email] JSON parse error: " + String(err.c_str()));
    return false;
  }

  JsonArray records = doc["list"];
  if (records.isNull()) {
    records = doc["data"].as<JsonArray>();
  }

  if (records.isNull() || records.size() == 0) {
    logLine("[Email] No records found");
    return false;
  }

  JsonObject rec = records[0];
  String email = rec["registered_email"].as<String>();
  if (email.length() == 0) {
    logLine("[Email] No email field in record");
    return false;
  }

  registeredEmail = email;
  preferences.putString("reg_email", registeredEmail);
  logLine("[Email] Email fetched and stored: " + registeredEmail);
  return true;
}

bool createCrossRecord() {
  if (WiFi.status() != WL_CONNECTED) return false;

  String url = String(NOCODB_BASE_URL) + String(NOCODB_CROSSES_PATH);

  StaticJsonDocument<512> doc;
  doc["name"] = DEVICE_NAME;
  doc["device_name"] = DEVICE_NAME;
  doc["device_key"] = DEVICE_KEY;
  doc["firmware_version"] = FIRMWARE_VERSION;
  doc["status"] = "active";
  doc["intercession_count"] = intercessionCount;
  
  // ALWAYS include email if we have one stored, regardless of when it was set
  if (registeredEmail.length() > 0) {
    doc["registered_email"] = registeredEmail;
    logLine("[NocoDB] Including email in cross creation: " + registeredEmail);
  } else {
    logLine("[NocoDB] No email available during cross creation");
  }

  String payload;
  serializeJson(doc, payload);
  logLine("[NocoDB] Create payload: " + payload);

  HTTPClient http;
  WiFiClientSecure client;
  client.setInsecure(); // TODO: add root CA for production
  http.begin(client, url);
  http.addHeader("Content-Type", "application/json");
  if (String(NOCODB_API_TOKEN).length() > 0) {
    http.addHeader("xc-token", NOCODB_API_TOKEN);
  }

  int code = http.POST(payload);
  logLine("[NocoDB] Create response code: " + String(code));
  
  if (code < 200 || code >= 300) {
    logLine("[NocoDB] Cross create failed code=" + String(code));
    String response = http.getString();
    logLine("[NocoDB] Create error: " + response);
    // If conflict or error, attempt to fetch existing (handles duplicate device_key)
    if (code == 409 || code == 400) {
      logLine("[NocoDB] Retry fetch after create conflict");
      http.end();
      return fetchExistingCross();
    }
    http.end();
    return false;
  }

  String response = http.getString();
  logLine("[NocoDB] Create response: " + response.substring(0, 200));

  DynamicJsonDocument resp(2048);
  DeserializationError err = deserializeJson(resp, response);
  http.end();
  if (err) {
    logLine("[NocoDB] Cross create JSON parse error: " + String(err.c_str()));
    return false;
  }

  String id = resp["Id"].as<String>();
  if (id.length() == 0 && !resp["data"].isNull()) {
    id = resp["data"]["Id"].as<String>();
  }
  if (id.length() == 0) {
    JsonArray arr = resp["list"].as<JsonArray>();
    if (!arr.isNull() && arr.size() > 0) {
      id = arr[0]["Id"].as<String>();
    }
  }

  if (id.length() == 0) {
    logLine("[NocoDB] Cross create missing Id");
    return false;
  }

  crossId = id;
  preferences.putString("cross_id", crossId);
  logLine("[NocoDB] Cross created id=" + crossId);
  return true;
}

bool verifyCrossIdExists() {
  if (WiFi.status() != WL_CONNECTED) return false;
  if (crossId.length() == 0 || crossId == "null") return false;

  String url = String(NOCODB_BASE_URL) + String(NOCODB_CROSSES_PATH) + "?where=(Id,eq," + crossId + ")";

  HTTPClient http;
  WiFiClientSecure client;
  client.setInsecure();
  http.begin(client, url);
  if (String(NOCODB_API_TOKEN).length() > 0) {
    http.addHeader("xc-token", NOCODB_API_TOKEN);
  }

  int code = http.GET();
  if (code != 200) {
    logLine("[NocoDB] verify id failed code=" + String(code));
    http.end();
    return false;
  }

  DynamicJsonDocument doc(2048);
  DeserializationError err = deserializeJson(doc, http.getString());
  http.end();
  if (err) return false;

  JsonArray records = doc["list"];
  if (records.isNull()) records = doc["data"].as<JsonArray>();
  if (records.isNull() || records.size() == 0) return false;
  return true;
}

// ============================================================================
// NOCODB POLLING (Prayers)
// ============================================================================

bool isCrossActive() {
  if (WiFi.status() != WL_CONNECTED) return false;
  if (crossId.length() == 0 || crossId == "null") return false;

  String url = String(NOCODB_BASE_URL) + String(NOCODB_CROSSES_PATH) + "?where=(Id,eq," + crossId + ")";

  HTTPClient http;
  WiFiClientSecure client;
  client.setInsecure();
  http.begin(client, url);
  if (String(NOCODB_API_TOKEN).length() > 0) {
    http.addHeader("xc-token", NOCODB_API_TOKEN);
  }

  int code = http.GET();
  if (code != 200) {
    logLine("[NocoDB] Failed to check cross status code=" + String(code));
    http.end();
    return false;
  }

  DynamicJsonDocument doc(2048);
  DeserializationError err = deserializeJson(doc, http.getString());
  http.end();

  if (err) {
    logLine("[NocoDB] Status check JSON parse error");
    return false;
  }

  JsonArray records = doc["list"];
  if (records.isNull()) {
    records = doc["data"].as<JsonArray>();
  }

  if (records.isNull() || records.size() == 0) {
    logLine("[NocoDB] Cross record not found");
    return false;
  }

  JsonObject rec = records[0];
  String status = rec["status"].as<String>();
  bool isActive = (status == "active");
  
  logLine("[NocoDB] Cross status: " + status + " (active=" + String(isActive) + ")");
  return isActive;
}

void pollAlertsIfDue() {
  unsigned long now = millis();
  if (now - lastAlertPollMs < ALERT_POLL_MS) return;
  lastAlertPollMs = now;
  pollAlerts();
}

void checkForOTAUpdateIfDue() {
  unsigned long now = millis();
  if (now - lastOTACheckMs < OTA_CHECK_INTERVAL_MS) return;
  lastOTACheckMs = now;
  checkForUpdate();
}

void pollAlerts() {
  if (WiFi.status() != WL_CONNECTED) return;
  if (crossId.length() == 0 || crossId == "null") return;

  // Check if this cross is active
  if (!isCrossActive()) {
    logLine("[CROSS] Not active, skipping alert poll");
    return;
  }

  // Update heartbeat
  updateCrossHeartbeat();

  // Fetch firing alerts from SchedKit
  String url = String(SCHEDKIT_BASE_URL) + "/v1/alerts?status=firing&limit=20";

  HTTPClient http;
  WiFiClientSecure client;
  client.setInsecure();
  http.begin(client, url);
  http.addHeader("x-api-key", SCHEDKIT_API_KEY);

  int code = http.GET();
  if (code != 200) {
    logLine("[SchedKit] Alert poll failed code=" + String(code));
    http.end();
    return;
  }

  DynamicJsonDocument doc(8192);
  DeserializationError err = deserializeJson(doc, http.getStream());
  http.end();

  if (err) {
    logLine("[SchedKit] JSON parse error: " + String(err.c_str()));
    return;
  }

  JsonArray alerts = doc["alerts"];
  if (alerts.isNull() || alerts.size() == 0) {
    logLine("[SchedKit] No firing alerts");
    return;
  }

  logLine("[SchedKit] Firing alerts: " + String(alerts.size()));

  int pulsed = 0;
  for (JsonObject alert : alerts) {
    long alertId = alert["Id"] | 0;
    String title = alert["title"] | "alert";
    String severity = alert["severity"] | "warning";

    logLine("[SchedKit] Alert id=" + String(alertId) + " sev=" + severity + " title=" + title);

    // Pulse LED for this alert
    runPrayerPulse();
    pulsed++;
    intercessionCount++;
    preferences.putLong("intercession_count", intercessionCount);

    // Ack the alert so we don't fire again
    String patchUrl = String(SCHEDKIT_BASE_URL) + "/v1/alerts/" + String(alertId);
    HTTPClient patchHttp;
    WiFiClientSecure patchClient;
    patchClient.setInsecure();
    patchHttp.begin(patchClient, patchUrl);
    patchHttp.addHeader("x-api-key", SCHEDKIT_API_KEY);
    patchHttp.addHeader("Content-Type", "application/json");
    int patchCode = patchHttp.PATCH("{\"status\":\"acked\"}");
    patchHttp.end();

    if (patchCode >= 200 && patchCode < 300) {
      logLine("[SchedKit] Alert " + String(alertId) + " acked");
    } else {
      logLine("[SchedKit] Ack failed code=" + String(patchCode));
    }

    if (pulsed < (int)alerts.size()) {
      delay(2000); // gap between pulses
    }
  }

  // Sync intercession count to NocoDB
  if (pulsed > 0) {
    updateCrossIntercessionCount();
  }
}

// ============================================================================
// API ENDPOINTS
// ============================================================================

void handleStatusAPI() {
  DynamicJsonDocument doc(256);
  doc["name"] = DEVICE_NAME;
  doc["version"] = FIRMWARE_VERSION;
  doc["mode"] = apModeActive ? "ap" : "station";
  doc["ip"] = apModeActive ? "192.168.4.1" : WiFi.localIP().toString();
  doc["wifi_saved"] = preferences.getString("wifi_ssid", "") != "";
  doc["email"] = registeredEmail;
  doc["intercession_count"] = intercessionCount;
  doc["heartbeat_count"] = heartbeatCount;

  String response;
  serializeJson(doc, response);

  server.sendHeader("Content-Type", "application/json");
  server.send(200, "application/json", response);
}

void handleLogsAPI() {
  if (!isAuthorized()) {
    server.send(401, "text/plain", "unauthorized");
    return;
  }

  String dump = buildLogDump();
  server.sendHeader("Content-Type", "text/plain");
  server.send(200, "text/plain", dump);
}

// ============================================================================
// SERIAL MAINTENANCE COMMANDS
// ============================================================================

void processSerialInput() {
  while (Serial.available() > 0) {
    char c = Serial.read();
    if (c == '\n' || c == '\r') {
      if (serialBuffer.length() > 0) {
        handleSerialCommand(serialBuffer);
        serialBuffer = "";
      }
    } else {
      if (serialBuffer.length() < 200) {
        serialBuffer += c;
      }
    }
  }
}

void handleSerialCommand(const String &line) {
  String cmd = line;
  cmd.trim();
  if (cmd.length() == 0) return;

  // Tokenize
  std::vector<String> parts;
  int start = 0;
  while (start < cmd.length()) {
    int space = cmd.indexOf(' ', start);
    if (space == -1) space = cmd.length();
    parts.push_back(cmd.substring(start, space));
    start = space + 1;
    while (start < cmd.length() && cmd[start] == ' ') start++;
  }

  if (parts.size() == 0) return;
  String op = parts[0];
  op.toLowerCase();

  if (op == "help") {
    Serial.println("Commands: help, setwifi <ssid> <password?>, resetwifi, setname <name>, settoken <token>, setemail");
    return;
  }

  if (op == "setwifi") {
    if (parts.size() < 2) {
      Serial.println("Usage: setwifi <ssid> <password?>");
      return;
    }
    String ssid = parts[1];
    String pwd = parts.size() >= 3 ? parts[2] : "";
    preferences.putString("wifi_ssid", ssid);
    preferences.putString("wifi_password", pwd);
    Serial.println("[SERIAL] WiFi saved, rebooting...");
    delay(200);
    ESP.restart();
    return;
  }

  if (op == "resetwifi") {
    preferences.putString("wifi_ssid", "");
    preferences.putString("wifi_password", "");
    Serial.println("[SERIAL] WiFi cleared, rebooting...");
    delay(200);
    ESP.restart();
    return;
  }

  if (op == "setname") {
    if (parts.size() < 2) {
      Serial.println("Usage: setname <device_name>");
      return;
    }
    String newName = parts[1];
    if (newName.length() > 32 || newName.length() == 0) {
      Serial.println("[SERIAL] Name must be 1-32 chars");
      return;
    }
    DEVICE_NAME = newName;
    preferences.putString("device_name", DEVICE_NAME);
    HOSTNAME = DEVICE_NAME + ".cross";
    MDNS_HOSTNAME = DEVICE_NAME + ".local";
    AP_SSID = DEVICE_NAME;
    Serial.println("[SERIAL] Name set; rebooting to apply");
    delay(200);
    ESP.restart();
    return;
  }

  if (op == "settoken") {
    if (parts.size() < 2) {
      Serial.println("Usage: settoken <token|empty>");
      return;
    }
    String tok = parts[1];
    if (tok.length() > 128) {
      Serial.println("[SERIAL] Token too long (max 128)");
      return;
    }
    apiToken = tok;
    preferences.putString("api_token", apiToken);
    Serial.println("[SERIAL] Token updated");
    return;
  }

  if (op == "setemail") {
    emailUpdateFlag = true;
    preferences.putBool("email_update_flag", true);
    Serial.println("[SERIAL] Email update flag set, rebooting...");
    delay(200);
    ESP.restart();
    return;
  }

  Serial.println("Unknown cmd; try 'help'");
}

void handleScanWiFiAPI() {
  logLine("scan wifi");

  int networks = WiFi.scanNetworks();
  DynamicJsonDocument doc(2048);
  JsonArray arr = doc.to<JsonArray>();

  for (int i = 0; i < networks; i++) {
    JsonObject net = arr.createNestedObject();
    net["ssid"] = WiFi.SSID(i);

    int sec = WiFi.encryptionType(i);
    String encryption = "open";
    if (sec == WIFI_AUTH_WEP) encryption = "WEP";
    else if (sec == WIFI_AUTH_WPA_PSK) encryption = "WPA";
    else if (sec == WIFI_AUTH_WPA2_PSK) encryption = "WPA2";
    else if (sec == WIFI_AUTH_WPA_WPA2_PSK) encryption = "WPA/WPA2";
    else if (sec == WIFI_AUTH_WPA2_ENTERPRISE) encryption = "WPA2-Enterprise";
    else if (sec == WIFI_AUTH_WPA3_PSK) encryption = "WPA3";

    net["encryption"] = encryption;
    net["rssi"] = WiFi.RSSI(i);
  }

  String response;
  serializeJson(arr, response);

  server.sendHeader("Content-Type", "application/json");
  server.send(200, "application/json", response);
}

// ============================================================================
// CAPTIVE PORTAL
// ============================================================================

void handlePortal() {
  String page;
  page.reserve(2048);
  page += "<!DOCTYPE html><html><head><meta charset='UTF-8'>";
  page += "<meta name='viewport' content='width=device-width,initial-scale=1'>";
  page += "<title>CROSS Setup</title>";
  page += "<style>body{font-family:sans-serif;background:#111;color:#eee;margin:0;padding:16px;}";
  page += ".card{background:#181818;border:1px solid #333;border-radius:8px;padding:16px;max-width:420px;width:100%;margin:auto;box-sizing:border-box;}";
  page += "label{display:block;margin:12px 0 6px;}input,select{width:100%;max-width:100%;padding:10px;border:1px solid #444;border-radius:6px;background:#000;color:#fff;box-sizing:border-box;}";
  page += "button{width:100%;max-width:100%;padding:12px;margin-top:14px;background:#fff;color:#000;border:none;border-radius:6px;font-weight:bold;box-sizing:border-box;}";
  page += "h1,h2,p{margin:0 0 12px 0;}</style></head><body><div class='card'>";
  page += "<h1>CROSS</h1><p>Configure WiFi</p>";

  int networks = WiFi.scanNetworks();
  page += "<form method='POST' action='/save'>";
  page += "<label>WiFi Network</label><select name='ssid' required>";
  if (networks <= 0) {
    page += "<option value=''>No networks found</option>";
  } else {
    for (int i = 0; i < networks; i++) {
      String ssid = WiFi.SSID(i);
      int rssi = WiFi.RSSI(i);
      page += "<option value='" + ssid + "'>" + ssid + " (" + String(rssi) + " dBm)</option>";
    }
  }
  page += "</select>";
  page += "<label>Password</label><input name='password' type='password' placeholder='optional if open'>";
  page += "<label>Email (Required)</label><input name='email' type='email' required placeholder='your@email.com'>";
  page += "<p style='font-size:11px;color:#888;margin:8px 0;'>Your email registers this cross to your account. All crosses you set up will be linked together. \"If anyone would come after me, let him deny himself and take up his cross and follow me.\" - Matthew 16:24</p>";
  page += "<button type='submit'>Save & Restart</button>";
  page += "</form><p style='margin-top:12px;font-size:12px;color:#aaa;'>Device: " + DEVICE_NAME + "</p>";
  page += "</div></body></html>";

  server.sendHeader("Content-Type", "text/html");
  server.send(200, "text/html", page);
}

void handlePortalSave() {
  String ssid = server.arg("ssid");
  String password = server.arg("password");
  String email = server.arg("email");

  if (ssid.length() == 0) {
    server.send(400, "text/plain", "SSID required");
    return;
  }

  if (email.length() == 0) {
    server.send(400, "text/plain", "Email required");
    return;
  }

  preferences.putString("wifi_ssid", ssid);
  preferences.putString("wifi_password", password);
  preferences.putString("reg_email", email);
  registeredEmail = email;
  // Verify persistence
  {
    String verify = preferences.getString("reg_email", "");
    if (verify != registeredEmail) {
      logLine("[Portal] Warning: reg_email verification failed, stored='" + verify + "'");
    } else {
      logLine("[Portal] reg_email persisted");
    }
  }

  String msg = "<html><body><h2>Saved.</h2><p>Rebooting...</p></body></html>";
  server.sendHeader("Content-Type", "text/html");
  server.send(200, "text/html", msg);
  delay(800);
  ESP.restart();
}

void handleConfigureWiFiAPI() {
  if (!server.hasArg("plain")) {
    server.send(400, "application/json", "{\"error\": \"No body\"}");
    return;
  }

  DynamicJsonDocument doc(256);
  DeserializationError err = deserializeJson(doc, server.arg("plain"));

  if (err) {
    server.send(400, "application/json", "{\"error\": \"Invalid JSON\"}");
    return;
  }

  String ssid = doc["ssid"] | "";
  String password = doc["password"] | "";
  String email = doc["email"] | ""; // optional email field supported for API provisioning

  if (ssid == "") {
    server.send(400, "application/json", "{\"error\": \"SSID required\"}");
    return;
  }

  logLine("store wifi ssid=" + ssid);
  preferences.putString("wifi_ssid", ssid);
  preferences.putString("wifi_password", password);

  // If the caller supplied an email, store it exactly like the captive portal does
  if (email.length() > 0) {
    logLine("[WiFi] Storing registered email from API: " + email);
    preferences.putString("reg_email", email);
    registeredEmail = email;
    // Verify persistence
    {
      String verify = preferences.getString("reg_email", "");
      if (verify != registeredEmail) {
        logLine("[WiFi] Warning: reg_email verification failed, stored='" + verify + "'");
      } else {
        logLine("[WiFi] reg_email persisted");
      }
    }
  }

  DynamicJsonDocument response(128);
  response["status"] = "configured";
  response["message"] = "Device will restart";

  String responseStr;
  serializeJson(response, responseStr);

  server.sendHeader("Content-Type", "application/json");
  server.send(200, "application/json", responseStr);

  delay(1000);
  ESP.restart();
}

void handleWiFiReset() {
  // Uncomment when ready to enforce auth:
  // if (!isAuthorized()) {
  //   server.send(401, "application/json", "{\"error\": \"Unauthorized\"}");
  //   return;
  // }
  
  logLine("WiFi reset requested");
  
  preferences.putString("wifi_ssid", "");
  preferences.putString("wifi_password", "");
  
  server.send(200, "application/json", "{\"status\": \"reset\", \"message\": \"WiFi credentials cleared, rebooting...\"}");
  delay(1000);
  ESP.restart();
}

bool isAuthorized() {
  // If no token is set, allow all requests (backward compatible)
  if (apiToken.length() == 0) return true;
  
  // Check for Authorization header
  if (!server.hasHeader("Authorization")) return false;
  
  String authHeader = server.header("Authorization");
  String expectedAuth = "Bearer " + apiToken;
  
  return authHeader == expectedAuth;
}

void handleSetToken() {
  if (!server.hasArg("plain")) {
    server.send(400, "application/json", "{\"error\": \"No body\"}");
    return;
  }

  DynamicJsonDocument doc(256);
  DeserializationError err = deserializeJson(doc, server.arg("plain"));

  if (err) {
    server.send(400, "application/json", "{\"error\": \"Invalid JSON\"}");
    return;
  }

  String newToken = doc["token"] | "";
  
  // Allow empty string to disable auth
  if (newToken.length() > 128) {
    server.send(400, "application/json", "{\"error\": \"token too long (max 128 chars)\"}");
    return;
  }

  logLine("[API] Setting API token");
  apiToken = newToken;
  preferences.putString("api_token", apiToken);
  
  DynamicJsonDocument response(128);
  response["status"] = "success";
  response["auth_enabled"] = (apiToken.length() > 0);
  
  String responseStr;
  serializeJson(response, responseStr);
  
  server.sendHeader("Content-Type", "application/json");
  server.send(200, "application/json", responseStr);
}

void handleSetEmail() {
  // No authorization required for local network usage
  String email = "";

  // Prefer JSON body when provided
  if (server.hasArg("plain") && server.arg("plain").length() > 0) {
    DynamicJsonDocument doc(256);
    DeserializationError err = deserializeJson(doc, server.arg("plain"));
    if (!err) {
      email = doc["email"] | "";
    } else {
      logLine("[API] Invalid JSON in /api/set-email: " + String(err.c_str()));
    }
  }

  // Fallback to form-encoded body (e.g., email=...)
  if (email.length() == 0 && server.hasArg("email")) {
    email = server.arg("email");
  }

  if (email == "" || email.length() > 128) {
    server.send(400, "application/json", "{\"error\": \"email required (max 128 chars)\"}");
    return;
  }

  logLine("[API] Setting registered email to: " + email);
  registeredEmail = email;

  // Write + diagnostics: check return bytes and do an explicit NVS reopen to force commit
  size_t written = preferences.putString("reg_email", registeredEmail);
  logLine("[API] preferences.putString wrote bytes=" + String(written));

  String verify = preferences.getString("reg_email", "");
  logLine("[API] verify immediately after putString='" + verify + "'");

  // Re-open preferences namespace to ensure commit is visible across opens
  preferences.end();
  delay(20);
  preferences.begin("cross", false);
  String verify2 = preferences.getString("reg_email", "");
  logLine("[API] verify after preferences.end()/begin()='" + verify2 + "'");

  if (verify2 != registeredEmail) {
    logLine("[API] Warning: reg_email verification failed after reopen, stored='" + verify2 + "'");
  } else {
    logLine("[API] reg_email persisted after reopen");
  }

  bool synced = false;
  if (WiFi.status() == WL_CONNECTED && crossId.length() > 0 && crossId != "null") {
    if (updateCrossEmailInNocoDB()) {
      synced = true;
    } else {
      logLine("[API] Email stored but NocoDB sync failed");
    }
  }

  DynamicJsonDocument response(128);
  response["status"] = "success";
  response["email"] = registeredEmail;
  response["synced"] = synced;

  String responseStr;
  serializeJson(response, responseStr);

  server.sendHeader("Content-Type", "application/json");
  server.send(200, "application/json", responseStr);
}

void handleReboot() {
  logLine("[API] Reboot requested via /api/reboot");
  DynamicJsonDocument response(128);
  response["status"] = "rebooting";
  String responseStr;
  serializeJson(response, responseStr);
  server.sendHeader("Content-Type", "application/json");
  server.send(200, "application/json", responseStr);
  delay(250);
  ESP.restart();
}

void handleSetName() {
  // Uncomment when ready to enforce auth:
  // if (!isAuthorized()) {
  //   server.send(401, "application/json", "{\"error\": \"Unauthorized\"}");
  //   return;
  // }
  
  if (!server.hasArg("plain")) {
    server.send(400, "application/json", "{\"error\": \"No body\"}");
    return;
  }

  DynamicJsonDocument doc(256);
  DeserializationError err = deserializeJson(doc, server.arg("plain"));

  if (err) {
    server.send(400, "application/json", "{\"error\": \"Invalid JSON\"}");
    return;
  }

  String newName = doc["device_name"] | "";
  if (newName == "" || newName.length() > 32) {
    server.send(400, "application/json", "{\"error\": \"device_name required (max 32 chars)\"}");
    return;
  }

  logLine("[API] Changing device name to: " + newName);
  
  // Update preferences and globals
  DEVICE_NAME = newName;
  preferences.putString("device_name", DEVICE_NAME);
  
  HOSTNAME = DEVICE_NAME + ".cross";
  MDNS_HOSTNAME = DEVICE_NAME + ".local";
  AP_SSID = DEVICE_NAME;
  
  // Update NocoDB if connected and registered
  if (WiFi.status() == WL_CONNECTED && crossId.length() > 0 && crossId != "null") {
    String url = String(NOCODB_BASE_URL) + String(NOCODB_CROSSES_PATH) + "?where=(Id,eq," + crossId + ")";
    StaticJsonDocument<256> updateDoc;
    updateDoc["Id"] = crossId;
    updateDoc["name"] = DEVICE_NAME;
    updateDoc["device_name"] = DEVICE_NAME;
    
    String payload;
    serializeJson(updateDoc, payload);
    
    HTTPClient http;
    WiFiClientSecure client;
    client.setInsecure();
    http.begin(client, url);
    http.addHeader("Content-Type", "application/json");
    if (String(NOCODB_API_TOKEN).length() > 0) {
      http.addHeader("xc-token", NOCODB_API_TOKEN);
    }
    
    int code = http.PATCH(payload);
    if (code >= 200 && code < 300) {
      logLine("[NocoDB] Device name updated");
    } else {
      logLine("[NocoDB] Name update failed code=" + String(code));
    }
    http.end();
  }
  
  DynamicJsonDocument response(128);
  response["status"] = "success";
  response["device_name"] = DEVICE_NAME;
  response["hostname"] = HOSTNAME;
  
  String responseStr;
  serializeJson(response, responseStr);
  
  server.sendHeader("Content-Type", "application/json");
  server.send(200, "application/json", responseStr);
}

// ============================================================================
// mDNS
// ============================================================================

void startmDNS() {
  if (MDNS.begin(DEVICE_NAME.c_str())) {
    MDNS.addService("http", "tcp", 80);
    logLine("mDNS http://" + MDNS_HOSTNAME + "/api/");
  }
}

// ============================================================================
// HTTP SERVER (shared AP/STA)
// ============================================================================

void startHttpServer(bool inAPMode) {
  if (serverStarted) return;

  if (inAPMode) {
    server.on("/", HTTP_GET, handlePortal);
    server.on("/save", HTTP_POST, handlePortalSave);
    server.onNotFound(handlePortal);
  }

  server.on("/api/status", HTTP_GET, handleStatusAPI);
  server.on("/api/scan-wifi", HTTP_GET, handleScanWiFiAPI);
  server.on("/api/configure-wifi", HTTP_POST, handleConfigureWiFiAPI);
  server.on("/api/reset-wifi", HTTP_POST, handleWiFiReset);
  server.on("/api/set-name", HTTP_POST, handleSetName);
  server.on("/api/set-token", HTTP_POST, handleSetToken);
  server.on("/api/set-email", HTTP_POST, handleSetEmail);
  server.on("/api/reboot", HTTP_POST, handleReboot);
  server.on("/api/logs", HTTP_GET, handleLogsAPI);
  server.on("/api/update", HTTP_POST, handleOTAUpdate);

  server.begin();
  serverStarted = true;
}

// ============================================================================
// OTA UPDATE
// ============================================================================

void handleOTAUpdate() {
  // Uncomment when ready to enforce auth:
  // if (!isAuthorized()) {
  //   server.send(401, "application/json", "{\"error\": \"Unauthorized\"}");
  //   return;
  // }
  
  logLine("OTA update requested");

  if (!isConnected) {
    server.send(400, "application/json", "{\"error\": \"Device must be on WiFi for OTA\"}");
    return;
  }

  server.send(200, "application/json", "{\"status\": \"updating\", \"message\": \"Downloading firmware...\"}");
  delay(100);

  checkForUpdate();
}

void checkForUpdate() {
  HTTPClient http;

  logLine("Checking for firmware update...");
  logLine("Current version: " + String(FIRMWARE_VERSION));

  http.begin(GITHUB_API_URL);
  http.addHeader("Accept", "application/vnd.github+json");
  int httpCode = http.GET();

  if (httpCode == 200) {
    DynamicJsonDocument doc(4096);
    DeserializationError err = deserializeJson(doc, http.getStream());

    if (!err) {
      String latestVersion = doc["tag_name"].as<String>();
      latestVersion.replace("v", "");

      logLine("Latest version: " + latestVersion);

      if (latestVersion != String(FIRMWARE_VERSION)) {
        logLine("New version available, finding firmware...");

        JsonArray assets = doc["assets"];
        String downloadUrl = "";

        for (JsonObject asset : assets) {
          String assetName = asset["name"].as<String>();
          if (assetName == "cross.bin") {
            downloadUrl = asset["browser_download_url"].as<String>();
            break;
          }
        }

        if (downloadUrl.length() > 0) {
          logLine("Downloading from: " + downloadUrl);
          http.end();

          // Use WiFiClientSecure for HTTPS with proper timeout
          WiFiClientSecure client;
          client.setInsecure(); // TODO: add root CA for production
          client.setConnectionTimeout(15000); // 15 second connect timeout
          client.setTimeout(30000); // 30 second socket timeout

          // Ensure redirects are followed for GitHub asset URLs
          httpUpdate.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);

          // Retry logic for download
          int maxRetries = 3;
          for (int attempt = 0; attempt < maxRetries; attempt++) {
            logLine("OTA download attempt " + String(attempt + 1) + "/" + String(maxRetries));
            
            t_httpUpdate_return ret = httpUpdate.update(client, downloadUrl);

            switch (ret) {
              case HTTP_UPDATE_FAILED:
                logLine("Update attempt " + String(attempt + 1) + " failed: " + String(httpUpdate.getLastErrorString()));
                if (attempt < maxRetries - 1) {
                  logLine("Retrying in 5 seconds...");
                  delay(5000);
                }
                break;
              case HTTP_UPDATE_NO_UPDATES:
                logLine("No update needed");
                return;
              case HTTP_UPDATE_OK:
                logLine("Update successful, rebooting...");
                // Mark version sync pending, ensure cross exists, then update DB before rebooting
                preferences.putBool("version_sync_pending", true);
                preferences.putString("version_sync_version", FIRMWARE_VERSION);
                registerCrossIfNeeded();
                updateCrossVersionInNocoDB();
                delay(1000);
                ESP.restart();
                break;
            }
            
            // Break on success or final failure
            if (ret != HTTP_UPDATE_FAILED || attempt == maxRetries - 1) {
              break;
            }
          }
        } else {
          logLine("Could not find cross.bin in release assets");
          http.end();
        }
      } else {
        logLine("Already on latest version");
        http.end();
      }
    } else {
      logLine("Failed to parse JSON response");
      http.end();
    }
  } else {
    logLine("Failed to check version: " + String(httpCode));
    http.end();
  }
}
