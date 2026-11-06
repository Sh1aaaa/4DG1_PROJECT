
#include <WiFi.h>
#include <HTTPClient.h>
#include <WebServer.h>
#include <ArduinoOTA.h>
#include <Preferences.h>
#include <LittleFS.h>
#include <ArduinoJson.h>
#include <DHT.h>
#include <time.h>
#include <math.h>

// ======================================================
// NODE 2 - CLASSROOM MONITORING SYSTEM
// ESP32 Arduino Core 3.x / ArduinoJson 7
// ======================================================

// -------------------- PINS ----------------------------

#define DHT_PIN       4
#define DHT_TYPE      DHT11
#define LDR_PIN       34
#define TRIG_PIN      26
#define ECHO_PIN      27
#define BUZZER_PIN    25

// -------------------- DEVICE --------------------------

const char* NODE_ID = "NODE_02";
const char* NODE_NAME = "Classroom";
const char* FIRMWARE_VERSION = "1.0.0";

// Previously provided Wi-Fi details.
// Change these if your network is different.

const char* DEFAULT_SSID = "301 TECH";
const char* DEFAULT_PASSWORD = "qwerty301!!!";

// Configuration access point

const char* AP_SSID = "Node2-Classroom-Setup";
const char* AP_PASSWORD = "Node2Setup2026";

// Default OTA password

const char* DEFAULT_OTA_PASSWORD = "Node2OTA2026";

// -------------------- TIMING --------------------------

const unsigned long SENSOR_INTERVAL = 3000;
const unsigned long REPORT_INTERVAL = 5000;
const unsigned long COMMAND_INTERVAL = 3000;
const unsigned long QUEUE_INTERVAL = 1200;
const unsigned long WIFI_RETRY_INTERVAL = 15000;

const size_t MAX_QUEUE_BYTES = 98304;

// -------------------- OBJECTS -------------------------

DHT dht(DHT_PIN, DHT_TYPE);

WebServer portal(80);
Preferences preferences;

// -------------------- CONFIGURATION ------------------

String wifiSSID;
String wifiPassword;
String serverHost;
String otaPassword;

uint16_t serverPort = 80;

int distanceThreshold = 50;
int temperatureThreshold = 35;

// -------------------- SENSOR DATA --------------------

float temperature = NAN;
float humidity = NAN;
float distanceCM = -1;

int lightRaw = 0;
int lightPercent = 0;

bool objectDetected = false;
bool temperatureAlert = false;
bool systemAlert = false;

// -------------------- BUZZER --------------------------

enum BuzzerMode {
  BUZZER_AUTO,
  BUZZER_FORCE_ON,
  BUZZER_FORCE_OFF
};

BuzzerMode buzzerMode = BUZZER_AUTO;

bool buzzerActive = false;

unsigned long beepUntil = 0;

String lastBeepCommand = "";

// -------------------- SYSTEM --------------------------

bool filesystemReady = false;
bool otaStarted = false;
bool wasConnected = false;

unsigned long lastSensorRead = 0;
unsigned long lastReport = 0;
unsigned long lastCommandCheck = 0;
unsigned long lastQueueCheck = 0;
unsigned long lastWiFiRetry = 0;

uint32_t queueOffset = 0;

// ======================================================
// HTML ESCAPING
// ======================================================

String htmlEscape(String value) {
  value.replace("&", "&amp;");
  value.replace("<", "&lt;");
  value.replace(">", "&gt;");
  value.replace("\"", "&quot;");
  value.replace("'", "&#39;");
  return value;
}

// ======================================================
// SAVE / LOAD CONFIGURATION
// ======================================================

void loadConfiguration() {

  preferences.begin("node2cfg", false);

  wifiSSID = preferences.getString(
    "ssid",
    DEFAULT_SSID
  );

  wifiPassword = preferences.getString(
    "password",
    DEFAULT_PASSWORD
  );

  serverHost = preferences.getString(
    "server",
    ""
  );

  otaPassword = preferences.getString(
    "otapass",
    DEFAULT_OTA_PASSWORD
  );

  serverPort = preferences.getUShort(
    "port",
    80
  );

  distanceThreshold = preferences.getInt(
    "distance",
    50
  );

  temperatureThreshold = preferences.getInt(
    "temp",
    35
  );

  queueOffset = preferences.getUInt(
    "offset",
    0
  );

  distanceThreshold = constrain(
    distanceThreshold, 5, 400
  );

  temperatureThreshold = constrain(
    temperatureThreshold, 20, 50
  );

  if (serverPort == 0) {
    serverPort = 80;
  }
}

// ======================================================
// BUZZER CONTROL
// ======================================================

void setBuzzerOutput(bool enabled) {

  if (enabled == buzzerActive) {
    return;
  }

  buzzerActive = enabled;

  if (enabled) {
    ledcWriteTone(BUZZER_PIN, 2200);
  } else {
    ledcWriteTone(BUZZER_PIN, 0);
  }
}

void updateBuzzer() {

  unsigned long now = millis();

  bool shouldSound = false;

  // Temporary one-second beep
  if ((int32_t)(beepUntil - now) > 0) {

    shouldSound = true;

  } else if (buzzerMode == BUZZER_FORCE_ON) {

    shouldSound = true;

  } else if (buzzerMode == BUZZER_FORCE_OFF) {

    shouldSound = false;

  } else {

    // Automatic alarm: beep intermittently
    if (systemAlert) {
      shouldSound = (now % 800) < 250;
    }
  }

  setBuzzerOutput(shouldSound);
}

// ======================================================
// ULTRASONIC DISTANCE SENSOR
// ======================================================

float readUltrasonic() {

  digitalWrite(TRIG_PIN, LOW);
  delayMicroseconds(2);

  digitalWrite(TRIG_PIN, HIGH);
  delayMicroseconds(10);

  digitalWrite(TRIG_PIN, LOW);

  unsigned long duration = pulseIn(
    ECHO_PIN,
    HIGH,
    25000UL
  );

  if (duration == 0) {
    return -1;
  }

  float cm = duration * 0.0343f / 2.0f;

  if (cm < 2 || cm > 400) {
    return -1;
  }

  return cm;
}

// ======================================================
// SENSOR READING
// ======================================================

void readSensors() {

  float newTemperature = dht.readTemperature();
  float newHumidity = dht.readHumidity();

  temperature = newTemperature;
  humidity = newHumidity;

  lightRaw = analogRead(LDR_PIN);

  // LDR connected to 3V3 with 10k resistor to GND.
  // Higher reading = brighter light.

  lightPercent = map(
    lightRaw,
    0,
    4095,
    0,
    100
  );

  distanceCM = readUltrasonic();

  objectDetected =
    distanceCM > 0 &&
    distanceCM <= distanceThreshold;

  temperatureAlert =
    !isnan(temperature) &&
    temperature >= temperatureThreshold;

  systemAlert =
    objectDetected || temperatureAlert;

  Serial.println();
  Serial.println("================================");

  Serial.println("NODE 2 - CLASSROOM");

  Serial.print("Temperature: ");
  if (isnan(temperature)) {
    Serial.println("Sensor error");
  } else {
    Serial.print(temperature);
    Serial.println(" C");
  }

  Serial.print("Humidity: ");
  if (isnan(humidity)) {
    Serial.println("Sensor error");
  } else {
    Serial.print(humidity);
    Serial.println(" %");
  }

  Serial.print("Light: ");
  Serial.print(lightPercent);
  Serial.println(" %");

  Serial.print("Distance: ");
  if (distanceCM < 0) {
    Serial.println("Out of range");
  } else {
    Serial.print(distanceCM);
    Serial.println(" cm");
  }

  Serial.print("Object Detected: ");
  Serial.println(objectDetected ? "YES" : "NO");

  Serial.print("Temperature Alert: ");
  Serial.println(temperatureAlert ? "YES" : "NO");

  Serial.print("System Alert: ");
  Serial.println(systemAlert ? "YES" : "NO");

  Serial.print("Wi-Fi: ");
  Serial.println(
    WiFi.status() == WL_CONNECTED
      ? "CONNECTED"
      : "DISCONNECTED"
  );

  Serial.println("================================");
}

// ======================================================
// DATE AND TIME
// ======================================================

String getTimestamp() {

  time_t now = time(nullptr);

  // Do not generate a fake timestamp if time
  // synchronization has not completed.

  if (now < 1700000000) {
    return "";
  }

  struct tm timeInfo;
  localtime_r(&now, &timeInfo);

  char timestamp[30];

  strftime(
    timestamp,
    sizeof(timestamp),
    "%Y-%m-%dT%H:%M:%S",
    &timeInfo
  );

  return String(timestamp);
}

// ======================================================
// CREATE SENSOR JSON
// ======================================================

String createSensorJSON() {

  JsonDocument doc;

  doc["node_id"] = NODE_ID;
  doc["node_name"] = NODE_NAME;

  String timestamp = getTimestamp();

  if (timestamp.length() > 0) {
    doc["timestamp"] = timestamp;
  } else {
    doc["timestamp"] = nullptr;
  }

  if (!isnan(temperature)) {
    doc["temperature"] = temperature;
  } else {
    doc["temperature"] = nullptr;
  }

  if (!isnan(humidity)) {
    doc["humidity"] = humidity;
  } else {
    doc["humidity"] = nullptr;
  }

  doc["light_raw"] = lightRaw;
  doc["light_percent"] = lightPercent;

  if (distanceCM >= 0) {
    doc["distance_cm"] = distanceCM;
  } else {
    doc["distance_cm"] = nullptr;
  }

  doc["presence"] = objectDetected;

  // Compatibility with example server payload.
  // This represents proximity, not PIR motion.
  doc["motion"] = objectDetected;

  doc["temperature_alert"] = temperatureAlert;
  doc["alert"] = systemAlert;

  doc["buzzer"] = buzzerActive;

  doc["rssi"] =
    WiFi.status() == WL_CONNECTED
      ? WiFi.RSSI()
      : 0;

  doc["firmware_version"] = FIRMWARE_VERSION;

  doc["uptime_ms"] = millis();

  String output;

  serializeJson(doc, output);

  return output;
}

// ======================================================
// PROCESS SERVER COMMANDS
// ======================================================

void processServerCommand(JsonDocument &doc) {

  String command = doc["command"].as<String>();

  command.trim();
  command.toLowerCase();

  if (command == "buzzer_on" ||
      command == "alarm_on") {

    buzzerMode = BUZZER_FORCE_ON;

    Serial.println("SERVER: Buzzer ON");
  }

  else if (command == "buzzer_off" ||
           command == "alarm_off" ||
           command == "silence") {

    buzzerMode = BUZZER_FORCE_OFF;

    beepUntil = 0;

    Serial.println("SERVER: Buzzer OFF");
  }

  else if (command == "buzzer_auto" ||
           command == "auto") {

    buzzerMode = BUZZER_AUTO;

    Serial.println("SERVER: Automatic mode");
  }

  else if (command == "beep") {

    String commandID = doc["command_id"].as<String>();

    if (commandID.length() == 0) {
      commandID = "beep";
    }

    if (commandID != lastBeepCommand) {

      lastBeepCommand = commandID;

      beepUntil = millis() + 1000;

      Serial.println("SERVER: Beep");
    }
  }

  if (command != "beep") {
    lastBeepCommand = "";
  }

  // Optional threshold configuration

  if (doc["distance_threshold_cm"].is<int>()) {

    int newThreshold = constrain(
      doc["distance_threshold_cm"].as<int>(),
      5,
      400
    );

    if (newThreshold != distanceThreshold) {

      distanceThreshold = newThreshold;

      preferences.putInt(
        "distance",
        distanceThreshold
      );
    }
  }

  if (doc["temperature_threshold_c"].is<int>()) {

    int newThreshold = constrain(
      doc["temperature_threshold_c"].as<int>(),
      20,
      50
    );

    if (newThreshold != temperatureThreshold) {

      temperatureThreshold = newThreshold;

      preferences.putInt(
        "temp",
        temperatureThreshold
      );
    }
  }

  objectDetected =
    distanceCM > 0 &&
    distanceCM <= distanceThreshold;

  temperatureAlert =
    !isnan(temperature) &&
    temperature >= temperatureThreshold;

  systemAlert =
    objectDetected || temperatureAlert;
}

// ======================================================
// SERVER URL
// ======================================================

String serverURL(const String &path) {

  return String("http://") +
         serverHost +
         ":" +
         String(serverPort) +
         path;
}

// ======================================================
// SEND HTTP POST
// ======================================================

bool sendDataToServer(const String &payload) {

  if (WiFi.status() != WL_CONNECTED) {
    return false;
  }

  if (serverHost.length() == 0) {
    return false;
  }

  WiFiClient client;
  HTTPClient http;

  if (!http.begin(
        client,
        serverURL("/api/data")
      )) {
    return false;
  }

  http.setTimeout(2000);

  http.addHeader(
    "Content-Type",
    "application/json"
  );

  int statusCode = http.POST(payload);

  bool accepted =
    statusCode >= 200 &&
    statusCode < 300;

  if (accepted) {

    String response = http.getString();

    if (response.length() > 0) {

      JsonDocument doc;

      DeserializationError error =
        deserializeJson(doc, response);

      if (!error) {

        if (doc["ack"].is<bool>() &&
            !doc["ack"].as<bool>()) {

          accepted = false;

        } else {

          processServerCommand(doc);
        }
      }
    }
  }

  http.end();

  if (accepted) {

    Serial.println("POST: Data sent successfully");

  } else {

    Serial.print("POST failed. HTTP code: ");
    Serial.println(statusCode);
  }

  return accepted;
}

// ======================================================
// GET COMMAND FROM SERVER
// ======================================================

void checkServerCommands() {

  if (WiFi.status() != WL_CONNECTED) {
    return;
  }

  if (serverHost.length() == 0) {
    return;
  }

  WiFiClient client;
  HTTPClient http;

  if (!http.begin(
        client,
        serverURL("/api/commands/node2")
      )) {
    return;
  }

  http.setTimeout(2000);

  int statusCode = http.GET();

  if (statusCode == 200) {

    String response = http.getString();

    JsonDocument doc;

    DeserializationError error =
      deserializeJson(doc, response);

    if (!error) {
      processServerCommand(doc);
    }
  }

  http.end();
}

// ======================================================
// LITTLEFS OFFLINE BUFFER
// ======================================================

bool hasBufferedData() {

  if (!filesystemReady) {
    return false;
  }

  if (!LittleFS.exists("/queue.jsonl")) {
    return false;
  }

  File file = LittleFS.open(
    "/queue.jsonl",
    "r"
  );

  if (!file) {
    return false;
  }

  bool result = file.size() > queueOffset;

  file.close();

  return result;
}

void saveOfflineData(const String &payload) {

  if (!filesystemReady) {

    Serial.println(
      "BUFFER ERROR: LittleFS unavailable"
    );

    return;
  }

  File file = LittleFS.open(
    "/queue.jsonl",
    "a"
  );

  if (!file) {

    Serial.println("BUFFER ERROR: Cannot open file");

    return;
  }

  if (file.size() + payload.length() + 1 >
      MAX_QUEUE_BYTES) {

    Serial.println(
      "BUFFER FULL: New reading not stored"
    );

    file.close();
    return;
  }

  file.println(payload);

  file.close();

  Serial.println("Reading saved to offline buffer");
}

// ======================================================
// RETRY ONE BUFFERED READING
// ======================================================

void retryBufferedData() {

  if (!hasBufferedData()) {
    return;
  }

  if (WiFi.status() != WL_CONNECTED ||
      serverHost.length() == 0) {
    return;
  }

  File file = LittleFS.open(
    "/queue.jsonl",
    "r"
  );

  if (!file) {
    return;
  }

  if (!file.seek(queueOffset)) {

    file.close();
    return;
  }

  String payload = file.readStringUntil('\n');

  uint32_t nextOffset = file.position();
  uint32_t fileSize = file.size();

  file.close();

  payload.trim();

  if (payload.length() == 0) {

    queueOffset = nextOffset;

    preferences.putUInt(
      "offset",
      queueOffset
    );

    return;
  }

  if (sendDataToServer(payload)) {

    Serial.println("Buffered data synchronized");

    if (nextOffset >= fileSize) {

      LittleFS.remove("/queue.jsonl");

      queueOffset = 0;

    } else {

      queueOffset = nextOffset;
    }

    preferences.putUInt(
      "offset",
      queueOffset
    );
  }
}

// ======================================================
// CONFIGURATION WEB PAGE
// ======================================================

void handleConfigurationPage() {

  String page = R"HTML(
<!DOCTYPE html>
<html>
<head>
<meta name="viewport"
content="width=device-width,initial-scale=1">
<title>Node 2 Configuration</title>
<style>
body{
  font-family:Arial,sans-serif;
  background:#0f172a;
  color:white;
  margin:0;
  padding:20px;
}
.container{
  max-width:520px;
  margin:20px auto;
  background:#1e293b;
  padding:25px;
  border-radius:14px;
}
h1{font-size:25px}
p{color:#cbd5e1}
label{
  display:block;
  margin-top:15px;
  margin-bottom:5px;
}
input{
  width:100%;
  box-sizing:border-box;
  padding:12px;
  border:1px solid #475569;
  border-radius:7px;
  background:#0f172a;
  color:white;
}
button,.button{
  display:inline-block;
  padding:12px 18px;
  margin-top:20px;
  background:#2563eb;
  border:0;
  border-radius:7px;
  color:white;
  text-decoration:none;
  cursor:pointer;
}
.info{
  background:#334155;
  padding:12px;
  border-radius:8px;
  margin-bottom:15px;
}
small{color:#94a3b8}
</style>
</head>
<body>
<div class="container">

<h1>Node 2 - Classroom</h1>

<div class="info">
  <p>Firmware: {{VERSION}}</p>
  <p>Wi-Fi: {{WIFI_STATUS}}</p>
  <p>Node IP: {{IP}}</p>
  <p>Server: {{SERVER_STATUS}}</p>
</div>

<form action="/save" method="POST">

<label>Wi-Fi SSID</label>
<input name="ssid"
value="{{SSID}}" required>

<label>Wi-Fi Password</label>
<input name="password"
type="password"
placeholder="Leave blank to keep existing">

<label>Central Server IP Address</label>
<input name="server"
value="{{SERVER}}"
placeholder="Example: 192.168.1.100">

<small>Enter the server address, not the
Node 2 address. Do not include http://</small>

<label>Server Port</label>
<input name="port"
type="number"
min="1"
max="65535"
value="{{PORT}}">

<label>Detection Distance (cm)</label>
<input name="distance"
type="number"
min="5"
max="400"
value="{{DISTANCE}}">

<label>Temperature Alert (C)</label>
<input name="temp"
type="number"
min="20"
max="50"
value="{{TEMP}}">

<label>OTA Password</label>
<input name="ota"
type="password"
placeholder="Leave blank to keep existing">

<button type="submit">Save and Restart</button>

</form>

<a class="button" href="/status">
View Sensor JSON
</a>

<a class="button" href="/beep">
Test Buzzer
</a>

</div>
</body>
</html>
)HTML";

  page.replace(
    "{{VERSION}}",
    FIRMWARE_VERSION
  );

  page.replace(
    "{{WIFI_STATUS}}",
    WiFi.status() == WL_CONNECTED
      ? "Connected"
      : "Disconnected"
  );

  page.replace(
    "{{IP}}",
    WiFi.status() == WL_CONNECTED
      ? WiFi.localIP().toString()
      : "Not connected"
  );

  page.replace(
    "{{SERVER_STATUS}}",
    serverHost.length() > 0
      ? htmlEscape(serverHost)
      : "Not configured"
  );

  page.replace(
    "{{SSID}}",
    htmlEscape(wifiSSID)
  );

  page.replace(
    "{{SERVER}}",
    htmlEscape(serverHost)
  );

  page.replace(
    "{{PORT}}",
    String(serverPort)
  );

  page.replace(
    "{{DISTANCE}}",
    String(distanceThreshold)
  );

  page.replace(
    "{{TEMP}}",
    String(temperatureThreshold)
  );

  portal.send(
    200,
    "text/html",
    page
  );
}

// ======================================================
// SAVE WEB CONFIGURATION
// ======================================================

void handleSaveConfiguration() {

  String newSSID = portal.arg("ssid");
  String newPassword = portal.arg("password");
  String newServer = portal.arg("server");
  String newOTA = portal.arg("ota");

  newSSID.trim();
  newServer.trim();

  if (newSSID.length() == 0) {

    portal.send(
      400,
      "text/plain",
      "Wi-Fi SSID is required"
    );

    return;
  }

  preferences.putString(
    "ssid",
    newSSID
  );

  if (newPassword.length() > 0) {

    preferences.putString(
      "password",
      newPassword
    );
  }

  preferences.putString(
    "server",
    newServer
  );

  int newPort = portal.arg("port").toInt();

  if (newPort < 1 || newPort > 65535) {
    newPort = 80;
  }

  preferences.putUShort(
    "port",
    (uint16_t)newPort
  );

  int newDistance = constrain(
    portal.arg("distance").toInt(),
    5,
    400
  );

  int newTemperature = constrain(
    portal.arg("temp").toInt(),
    20,
    50
  );

  preferences.putInt(
    "distance",
    newDistance
  );

  preferences.putInt(
    "temp",
    newTemperature
  );

  if (newOTA.length() >= 8) {

    preferences.putString(
      "otapass",
      newOTA
    );
  }

  portal.send(
    200,
    "text/html",
    "<h2>Settings saved!</h2>"
    "<p>ESP32 restarting...</p>"
  );

  delay(1000);

  ESP.restart();
}

// ======================================================
// START CONFIGURATION PORTAL
// ======================================================

void startConfigurationPortal() {

  WiFi.mode(WIFI_AP_STA);

  WiFi.softAP(
    AP_SSID,
    AP_PASSWORD
  );

  Serial.println();
  Serial.println("CONFIGURATION PORTAL");

  Serial.print("AP Name: ");
  Serial.println(AP_SSID);

  Serial.print("AP IP: ");
  Serial.println(WiFi.softAPIP());

  portal.on(
    "/",
    HTTP_GET,
    handleConfigurationPage
  );

  portal.on(
    "/save",
    HTTP_POST,
    handleSaveConfiguration
  );

  portal.on(
    "/status",
    HTTP_GET,
    []() {

      portal.send(
        200,
        "application/json",
        createSensorJSON()
      );
    }
  );

  portal.on(
    "/beep",
    HTTP_GET,
    []() {

      beepUntil = millis() + 1000;

      portal.sendHeader(
        "Location",
        "/"
      );

      portal.send(
        303,
        "text/plain",
        "Beep"
      );
    }
  );

  portal.onNotFound(
    []() {

      portal.sendHeader(
        "Location",
        "/"
      );

      portal.send(
        302,
        "text/plain",
        "Redirecting"
      );
    }
  );

  portal.begin();

  Serial.println("Configuration web server started");
}

// ======================================================
// SETUP OTA
// ======================================================

void startOTA() {

  if (otaStarted) {
    return;
  }

  ArduinoOTA.setHostname(
    "node2-classroom"
  );

  ArduinoOTA.setPassword(
    otaPassword.c_str()
  );

  ArduinoOTA.onStart(
    []() {

      setBuzzerOutput(false);

      Serial.println("OTA update started");
    }
  );

  ArduinoOTA.onEnd(
    []() {

      Serial.println("OTA update completed");
    }
  );

  ArduinoOTA.onError(
    [](ota_error_t error) {

      Serial.print("OTA error: ");
      Serial.println((int)error);
    }
  );

  ArduinoOTA.begin();

  otaStarted = true;

  Serial.println("Arduino OTA is ready");
}

// ======================================================
// WIFI MANAGEMENT
// ======================================================

void maintainWiFi() {

  bool connected =
    WiFi.status() == WL_CONNECTED;

  if (connected && !wasConnected) {

    Serial.println();
    Serial.println("Wi-Fi connected!");

    Serial.print("IP address: ");
    Serial.println(WiFi.localIP());

    configTime(
      8 * 3600,
      0,
      "pool.ntp.org",
      "time.google.com"
    );

    startOTA();
  }

  if (!connected && wasConnected) {

    Serial.println("Wi-Fi disconnected");

    Serial.println(
      "Sensor readings will be buffered"
    );
  }

  wasConnected = connected;

  if (!connected &&
      millis() - lastWiFiRetry >= WIFI_RETRY_INTERVAL) {

    lastWiFiRetry = millis();

    if (wifiSSID.length() > 0) {

      Serial.println("Reconnecting to Wi-Fi...");

      WiFi.reconnect();
    }
  }
}

// ======================================================
// ESP32 SETUP
// ======================================================

void setup() {

  Serial.begin(115200);

  delay(500);

  Serial.println();
  Serial.println("=================================");
  Serial.println("NODE 2 - CLASSROOM MONITORING");
  Serial.println("=================================");

  pinMode(TRIG_PIN, OUTPUT);
  pinMode(ECHO_PIN, INPUT);
  pinMode(LDR_PIN, INPUT);

  digitalWrite(TRIG_PIN, LOW);

  // Passive buzzer PWM, Arduino Core 3.x

  if (!ledcAttach(
        BUZZER_PIN,
        2200,
        8
      )) {

    Serial.println(
      "ERROR: Buzzer PWM initialization failed"
    );
  }

  ledcWriteTone(BUZZER_PIN, 0);

  // Initialize DHT11

  dht.begin();

  analogReadResolution(12);

  // Load saved configuration

  loadConfiguration();

  // Initialize offline storage

  filesystemReady = LittleFS.begin(true);

  if (filesystemReady) {

    Serial.println("LittleFS initialized");

    if (!LittleFS.exists("/queue.jsonl")) {

      queueOffset = 0;

      preferences.putUInt(
        "offset",
        0
      );
    }

  } else {

    Serial.println("LittleFS initialization failed");
  }

  // Start configuration access point

  startConfigurationPortal();

  // Connect to existing Wi-Fi

  WiFi.setAutoReconnect(true);

  WiFi.setHostname(
    "node2-classroom"
  );

  if (wifiSSID.length() > 0) {

    Serial.print("Connecting to: ");
    Serial.println(wifiSSID);

    WiFi.begin(
      wifiSSID.c_str(),
      wifiPassword.c_str()
    );
  }

  lastWiFiRetry = millis();

  Serial.println();
  Serial.println("NODE 2 INITIALIZED");

  Serial.println(
    "Setup AP: Node2-Classroom-Setup"
  );

  Serial.println(
    "Setup URL: http://192.168.4.1"
  );

  Serial.println("Firmware: 1.0.0");
}

// ======================================================
// MAIN LOOP
// ======================================================

void loop() {

  unsigned long now = millis();

  // Process configuration portal requests

  portal.handleClient();

  // Maintain network connection

  maintainWiFi();

  // Handle OTA updates

  if (otaStarted &&
      WiFi.status() == WL_CONNECTED) {

    ArduinoOTA.handle();
  }

  // Read sensors periodically

  if (now - lastSensorRead >= SENSOR_INTERVAL) {

    lastSensorRead = now;

    readSensors();
  }

  // Update buzzer without blocking

  updateBuzzer();

  // Send sensor readings to server

  if (now - lastReport >= REPORT_INTERVAL) {

    lastReport = now;

    String payload = createSensorJSON();

    if (hasBufferedData()) {

      // Keep queued readings in order
      saveOfflineData(payload);

    } else {

      if (!sendDataToServer(payload)) {

        saveOfflineData(payload);
      }
    }
  }

  // Receive server commands

  if (now - lastCommandCheck >= COMMAND_INTERVAL) {

    lastCommandCheck = now;

    checkServerCommands();
  }

  // Retry buffered readings

  if (now - lastQueueCheck >= QUEUE_INTERVAL) {

    lastQueueCheck = now;

    retryBufferedData();
  }

  delay(2);
}
