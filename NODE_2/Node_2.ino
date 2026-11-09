/*
  NODE 1 - ENTRANCE
  Firmware: 2.1.0

  Features:
  - Wi-Fi connection
  - ArduinoOTA firmware updates
  - DHT11 temperature and humidity
  - HC-SR04 distance and presence detection
  - Automatic and manual door control
  - Green and red LED indicators
  - Central Server data transmission
  - Command polling
  - LittleFS offline data queue
*/

#include <WiFi.h>
#include <HTTPClient.h>
#include <WebServer.h>
#include <ArduinoOTA.h>
#include <Preferences.h>
#include <LittleFS.h>
#include <ArduinoJson.h>
#include <DHT.h>
#include <ESP32Servo.h>
#include <time.h>

// ==================== CONFIGURATION ====================

#define NODE_ID       "NODE_01"
#define NODE_NAME     "Entrance"
#define FIRMWARE      "2.1.0"

const char* WIFI_SSID = "301 TECH";
const char* WIFI_PASS = "qwerty301!!!";

// CHANGE THIS TO YOUR CENTRAL SERVER IP ADDRESS
const char* SERVER_URL = "http://192.168.1.100";

const char* OTA_HOSTNAME = "node1-entrance";
const char* OTA_PASSWORD = "Node1OTA2026";

const char* QUEUE_FILE = "/node1_queue.jsonl";

#define DHT_PIN       4
#define DHT_TYPE      DHT11

#define TRIG_PIN      5
#define ECHO_PIN      18

#define SERVO_PIN     13
#define GREEN_LED     26
#define RED_LED       27

// ==================== OBJECTS ====================

DHT dht(DHT_PIN, DHT_TYPE);
Servo doorServo;

WebServer configServer(80);
Preferences preferences;

// ==================== VARIABLES ====================

float temperature = NAN;
float humidity = NAN;
float distanceCM = -1;

bool presence = false;
bool doorOpen = false;
bool automaticMode = true;

int doorOpenAngle = 90;
int doorClosedAngle = 0;

unsigned long lastSensorRead = 0;
unsigned long lastDataSend = 0;
unsigned long lastCommandCheck = 0;
unsigned long lastReconnect = 0;
unsigned long doorOpenedAt = 0;

const unsigned long SENSOR_INTERVAL = 2500;
const unsigned long DATA_INTERVAL = 3000;
const unsigned long COMMAND_INTERVAL = 1500;
const unsigned long DOOR_OPEN_TIME = 4000;

// ==================== WIFI ====================

void connectWiFi() {
  Serial.println();
  Serial.println("Connecting to Wi-Fi...");

  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASS);

  unsigned long start = millis();

  while (WiFi.status() != WL_CONNECTED &&
         millis() - start < 20000) {
    delay(500);
    Serial.print(".");
  }

  if (WiFi.status() == WL_CONNECTED) {
    Serial.println("\nWi-Fi connected!");
    Serial.print("IP Address: ");
    Serial.println(WiFi.localIP());
  } else {
    Serial.println("\nWi-Fi connection failed.");
  }
}

// ==================== OTA ====================

void setupOTA() {
  ArduinoOTA.setHostname(OTA_HOSTNAME);
  ArduinoOTA.setPassword(OTA_PASSWORD);

  ArduinoOTA.onStart([]() {
    Serial.println("Node 1 OTA update started.");
  });

  ArduinoOTA.onEnd([]() {
    Serial.println("\nNode 1 OTA update completed.");
  });

  ArduinoOTA.onProgress([](unsigned int progress,
                           unsigned int total) {
    Serial.printf("OTA Progress: %u%%\r",
                  (progress * 100) / total);
  });

  ArduinoOTA.onError([](ota_error_t error) {
    Serial.printf("OTA Error: %u\n", error);
  });

  ArduinoOTA.begin();

  Serial.println("Node 1 OTA Ready.");
}

// ==================== TIME ====================

void setupTime() {
  configTime(8 * 3600, 0, "pool.ntp.org",
             "time.google.com");
}

String getTimestamp() {
  struct tm timeInfo;

  if (!getLocalTime(&timeInfo, 100)) {
    return "unsynchronized";
  }

  char buffer[30];

  strftime(buffer, sizeof(buffer),
           "%Y-%m-%d %H:%M:%S", &timeInfo);

  return String(buffer);
}

// ==================== DISTANCE SENSOR ====================

float readDistance() {
  digitalWrite(TRIG_PIN, LOW);
  delayMicroseconds(2);

  digitalWrite(TRIG_PIN, HIGH);
  delayMicroseconds(10);

  digitalWrite(TRIG_PIN, LOW);

  unsigned long duration =
    pulseIn(ECHO_PIN, HIGH, 30000);

  if (duration == 0) {
    return -1;
  }

  return duration * 0.0343 / 2.0;
}

// ==================== SENSOR READING ====================

void readSensors() {
  if (millis() - lastSensorRead < SENSOR_INTERVAL) {
    return;
  }

  lastSensorRead = millis();

  float newTemperature = dht.readTemperature();
  float newHumidity = dht.readHumidity();

  if (!isnan(newTemperature)) {
    temperature = newTemperature;
  }

  if (!isnan(newHumidity)) {
    humidity = newHumidity;
  }

  distanceCM = readDistance();

  presence = (
    distanceCM > 0 &&
    distanceCM <= 100
  );

  Serial.println("\n--- NODE 1 SENSOR DATA ---");

  Serial.print("Temperature: ");
  Serial.println(temperature);

  Serial.print("Humidity: ");
  Serial.println(humidity);

  Serial.print("Distance: ");
  Serial.println(distanceCM);

  Serial.print("Presence: ");
  Serial.println(presence);
}

// ==================== DOOR CONTROL ====================

void openDoor() {
  doorServo.write(doorOpenAngle);

  doorOpen = true;
  doorOpenedAt = millis();

  digitalWrite(GREEN_LED, HIGH);
  digitalWrite(RED_LED, LOW);

  Serial.println("Door opened.");
}

void closeDoor() {
  doorServo.write(doorClosedAngle);

  doorOpen = false;

  digitalWrite(GREEN_LED, LOW);
  digitalWrite(RED_LED, HIGH);

  Serial.println("Door closed.");
}

void handleAutomaticDoor() {
  if (!automaticMode) {
    return;
  }

  if (presence && !doorOpen) {
    openDoor();
  }

  if (doorOpen &&
      !presence &&
      millis() - doorOpenedAt >= DOOR_OPEN_TIME) {
    closeDoor();
  }
}

// ==================== JSON DATA ====================

String createPayload() {
  JsonDocument doc;

  doc["node_id"] = NODE_ID;
  doc["node_name"] = NODE_NAME;

  if (!isnan(temperature)) {
    doc["temperature"] = temperature;
  }

  if (!isnan(humidity)) {
    doc["humidity"] = humidity;
  }

  if (distanceCM >= 0) {
    doc["distance_cm"] = distanceCM;
  } else {
    doc["distance_cm"] = nullptr;
  }

  doc["motion"] = presence;
  doc["presence"] = presence;

  doc["door"] = doorOpen ? "open" : "closed";

  doc["rssi"] = WiFi.RSSI();
  doc["firmware_version"] = FIRMWARE;
  doc["fw"] = FIRMWARE;
  doc["uptime_ms"] = millis();
  doc["timestamp"] = getTimestamp();

  String payload;
  serializeJson(doc, payload);

  return payload;
}

// ==================== OFFLINE STORAGE ====================

void saveOfflineData(String payload) {
  File file = LittleFS.open(QUEUE_FILE, FILE_APPEND);

  if (!file) {
    Serial.println("Unable to save offline data.");
    return;
  }

  file.println(payload);
  file.close();

  Serial.println("Data saved to offline queue.");
}

bool sendPayload(String payload) {
  if (WiFi.status() != WL_CONNECTED) {
    return false;
  }

  HTTPClient http;

  http.begin(String(SERVER_URL) + "/api/data");
  http.addHeader("Content-Type", "application/json");
  http.setTimeout(3000);

  int responseCode = http.POST(payload);

  http.end();

  if (responseCode >= 200 && responseCode < 300) {
    Serial.println("Data sent to Central Server.");
    return true;
  }

  Serial.printf("Server response: %d\n", responseCode);
  return false;
}

void flushOfflineData() {
  if (!LittleFS.exists(QUEUE_FILE) ||
      WiFi.status() != WL_CONNECTED) {
    return;
  }

  File file = LittleFS.open(QUEUE_FILE, FILE_READ);

  if (!file) {
    return;
  }

  String remaining = "";
  bool failed = false;

  while (file.available()) {
    String line = file.readStringUntil('\n');
    line.trim();

    if (line.length() == 0) {
      continue;
    }

    if (!failed && sendPayload(line)) {
      continue;
    }

    failed = true;
    remaining += line + "\n";
  }

  file.close();

  File newFile = LittleFS.open(QUEUE_FILE, FILE_WRITE);

  if (newFile) {
    newFile.print(remaining);
    newFile.close();
  }
}

// ==================== DATA TRANSMISSION ====================

void transmitData() {
  if (millis() - lastDataSend < DATA_INTERVAL) {
    return;
  }

  lastDataSend = millis();

  String payload = createPayload();

  if (!sendPayload(payload)) {
    saveOfflineData(payload);
  } else {
    flushOfflineData();
  }
}

// ==================== SERVER COMMANDS ====================

void executeCommand(String command) {
  command.trim();
  command.toLowerCase();

  Serial.print("Received command: ");
  Serial.println(command);

  if (command == "door_open") {
    automaticMode = false;
    openDoor();
  }

  else if (command == "door_close") {
    automaticMode = false;
    closeDoor();
  }

  else if (command == "auto_on") {
    automaticMode = true;
  }

  else if (command == "auto_off") {
    automaticMode = false;
  }

  else if (command == "reboot") {
    ESP.restart();
  }
}

void processCommandResponse(String response) {
  JsonDocument doc;

  DeserializationError error =
    deserializeJson(doc, response);

  if (error) {
    Serial.println("Invalid command response.");
    return;
  }

  if (doc["commands"].is<JsonArray>()) {
    JsonArray commands = doc["commands"].as<JsonArray>();

    for (JsonVariant item : commands) {
      if (item.is<JsonObject>()) {
        String command = item["command"] |
                         item["action"] | "";

        if (command.length() > 0) {
          executeCommand(command);
        }
      } else if (item.is<const char*>()) {
        executeCommand(item.as<String>());
      }
    }
  }

  else if (doc["command"].is<const char*>()) {
    executeCommand(doc["command"].as<String>());
  }
}

void pollCommands() {
  if (WiFi.status() != WL_CONNECTED) {
    return;
  }

  if (millis() - lastCommandCheck < COMMAND_INTERVAL) {
    return;
  }

  lastCommandCheck = millis();

  HTTPClient http;

  http.begin(String(SERVER_URL) + "/api/commands/node1");
  http.setTimeout(2000);

  int responseCode = http.GET();

  if (responseCode == 200) {
    processCommandResponse(http.getString());
  }

  http.end();
}

// ==================== WIFI RECONNECTION ====================

void maintainWiFi() {
  if (WiFi.status() == WL_CONNECTED) {
    return;
  }

  if (millis() - lastReconnect >= 10000) {
    lastReconnect = millis();

    Serial.println("Reconnecting to Wi-Fi...");

    WiFi.disconnect();
    WiFi.begin(WIFI_SSID, WIFI_PASS);
  }
}

// ==================== SETUP ====================

void setup() {
  Serial.begin(115200);

  pinMode(TRIG_PIN, OUTPUT);
  pinMode(ECHO_PIN, INPUT);

  pinMode(GREEN_LED, OUTPUT);
  pinMode(RED_LED, OUTPUT);

  digitalWrite(GREEN_LED, LOW);
  digitalWrite(RED_LED, HIGH);

  dht.begin();

  doorServo.setPeriodHertz(50);
  doorServo.attach(SERVO_PIN, 500, 2400);
  closeDoor();

  if (!LittleFS.begin(true)) {
    Serial.println("LittleFS initialization failed.");
  }

  connectWiFi();

  if (WiFi.status() == WL_CONNECTED) {
    setupTime();
    setupOTA();
  }

  Serial.println("NODE 1 - ENTRANCE READY");
}

// ==================== MAIN LOOP ====================

void loop() {
  if (WiFi.status() == WL_CONNECTED) {
    ArduinoOTA.handle();
  }

  maintainWiFi();

  readSensors();

  handleAutomaticDoor();

  transmitData();

  pollCommands();

  delay(10);
}