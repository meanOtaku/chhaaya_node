#include <Arduino.h>
#include <WiFi.h>
#include <AsyncTCP.h>
#include <ESPAsyncWebServer.h>
#include <AsyncMqttClient.h>
#include <ArduinoJson.h>
#include "LittleFS.h"
#include "time.h"
#include <Wire.h>
#include <MPU9250.h>

/* ===================== CONFIG ===================== */

#define MQTT_PORT 1883
#define AP_SSID "ESP-WIFI-MANAGER"
#define LED_PIN 2

const char* ntpServer = "pool.ntp.org";
const long  gmtOffset_sec = 19800;
const int   daylightOffset_sec = 0;

/* ===================== IMU ===================== */

MPU9250 imu(Wire, 0x68);

/* ===================== OBJECTS ===================== */

AsyncWebServer server(80);
AsyncMqttClient mqttClient;
StaticJsonDocument<768> jsonDoc;

/* ===================== GLOBALS ===================== */

String ssid, pass, ip, gateway;
String mqtt_ip;

String deviceId;
String statusTopic;
String controlTopic;
String imuTopic;

String role = "left_leg";  // 🔥 CHANGE PER DEVICE

IPAddress localIP, localGateway;
IPAddress subnet(255, 255, 0, 0);

unsigned long lastStatus = 0;
unsigned long statusInterval = 30000;

/* ===================== FILESYSTEM ===================== */

String readFile(const char* path) {
  if (!LittleFS.exists(path)) return "";
  File f = LittleFS.open(path);
  String v = f.readStringUntil('\n');
  f.close();
  return v;
}

/* ===================== TIME ===================== */

void initTime() {
  configTime(gmtOffset_sec, daylightOffset_sec, ntpServer);

  struct tm timeinfo;
  int retry = 0;

  while (!getLocalTime(&timeinfo) && retry < 10) {
    delay(1000);
    retry++;
  }
}

long long getTimestampMs() {
  struct timeval tv;
  gettimeofday(&tv, NULL);
  return (long long)tv.tv_sec * 1000LL + (tv.tv_usec / 1000);
}

/* ===================== IMU ===================== */

void initIMU() {
  Wire.begin();

  int status = imu.begin();
  if (status < 0) {
    Serial.println("❌ IMU init failed");
    while (1);
  }

  Serial.println("✅ IMU ready");
}

void readIMU(float &ax, float &ay, float &az,
             float &gx, float &gy, float &gz,
             float &mx, float &my, float &mz) {

  imu.readSensor();

  ax = imu.getAccelX_mss();
  ay = imu.getAccelY_mss();
  az = imu.getAccelZ_mss();

  gx = imu.getGyroX_rads();
  gy = imu.getGyroY_rads();
  gz = imu.getGyroZ_rads();

  mx = imu.getMagX_uT();
  my = imu.getMagY_uT();
  mz = imu.getMagZ_uT();
}

/* ===================== MQTT ===================== */

void connectMqtt() {
  if (!mqttClient.connected()) {
    mqttClient.connect();
  }
}

void publishStatus(const char* state) {
  if (!mqttClient.connected()) return;

  jsonDoc.clear();
  jsonDoc["device"] = deviceId;
  jsonDoc["state"]  = state;
  jsonDoc["ts"]     = getTimestampMs();

  char buffer[256];
  serializeJson(jsonDoc, buffer);

  mqttClient.publish(statusTopic.c_str(), 1, true, buffer);
}

void publishIMU() {
  if (!mqttClient.connected()) return;

  float ax, ay, az, gx, gy, gz, mx, my, mz;
  readIMU(ax, ay, az, gx, gy, gz, mx, my, mz);

  jsonDoc.clear();

  jsonDoc["device"] = deviceId;
  jsonDoc["role"]   = role;
  jsonDoc["ts"]     = getTimestampMs();

  jsonDoc["ax"] = ax;
  jsonDoc["ay"] = ay;
  jsonDoc["az"] = az;

  jsonDoc["gx"] = gx;
  jsonDoc["gy"] = gy;
  jsonDoc["gz"] = gz;

  jsonDoc["mx"] = mx;
  jsonDoc["my"] = my;
  jsonDoc["mz"] = mz;

  char buffer[768];
  serializeJson(jsonDoc, buffer);

  mqttClient.publish(imuTopic.c_str(), 0, false, buffer);
}

void onMqttConnect(bool) {
  Serial.println("✅ MQTT CONNECTED");
  publishStatus("online");
  mqttClient.subscribe(controlTopic.c_str(), 1);
}

void onMqttDisconnect(AsyncMqttClientDisconnectReason) {
  if (WiFi.isConnected()) {
    delay(2000);
    mqttClient.connect();
  }
}

void onMqttMessage(char* topic, char* payload,
                   AsyncMqttClientMessageProperties,
                   size_t len, size_t, size_t) {

  String msg;
  for (size_t i = 0; i < len; i++) msg += payload[i];

  if (String(topic) == controlTopic) {
    if (msg == "on") digitalWrite(LED_PIN, HIGH);
    else if (msg == "off") digitalWrite(LED_PIN, LOW);
  }
}

/* ===================== WIFI ===================== */

bool connectWiFi() {
  if (ssid.isEmpty() || ip.isEmpty()) return false;

  WiFi.mode(WIFI_STA);
  localIP.fromString(ip);
  localGateway.fromString(gateway);

  if (!WiFi.config(localIP, localGateway, subnet)) return false;

  WiFi.begin(ssid.c_str(), pass.c_str());

  unsigned long start = millis();
  while (WiFi.status() != WL_CONNECTED) {
    if (millis() - start > 10000) return false;
    delay(300);
  }

  return true;
}

/* ===================== SETUP ===================== */

void setup() {
  Serial.begin(115200);
  pinMode(LED_PIN, OUTPUT);

  LittleFS.begin(true);

  ssid    = readFile("/ssid.txt");
  pass    = readFile("/pass.txt");
  ip      = readFile("/ip.txt");
  gateway = readFile("/gateway.txt");
  mqtt_ip = readFile("/mqtt_ip.txt");

  uint64_t chipid = ESP.getEfuseMac();
  deviceId = String((uint32_t)(chipid >> 32), HEX) +
             String((uint32_t)chipid, HEX);

  statusTopic  = "devices/" + deviceId + "/status";
  controlTopic = "devices/" + deviceId + "/control";
  imuTopic     = "session/default/imu";

  mqttClient.onConnect(onMqttConnect);
  mqttClient.onDisconnect(onMqttDisconnect);
  mqttClient.onMessage(onMqttMessage);

  mqttClient.setServer(mqtt_ip.c_str(), MQTT_PORT);

  mqttClient.setWill(
    statusTopic.c_str(),
    1,
    true,
    "{\"state\":\"offline\"}"
  );

  if (connectWiFi()) {
    initTime();
    initIMU();
    connectMqtt();
  } else {
    WiFi.softAP(AP_SSID);
  }

  statusInterval = random(25000, 35000);
}

/* ===================== LOOP ===================== */

void loop() {

  // WiFi reconnect
  if (WiFi.status() != WL_CONNECTED) {
    if (connectWiFi()) {
      initTime();
      connectMqtt();
    }
  }

  // MQTT reconnect
  if (WiFi.isConnected() && !mqttClient.connected()) {
    connectMqtt();
  }

  // Status publish
  if (WiFi.isConnected() && millis() - lastStatus > statusInterval) {
    lastStatus = millis();
    publishStatus("online");
  }

  // IMU publish (~50 Hz)
  static unsigned long lastIMU = 0;
  if (millis() - lastIMU > 20) {
    lastIMU = millis();
    publishIMU();
  }

  // periodic NTP sync (~10 min)
  static unsigned long lastNtp = 0;
  if (millis() - lastNtp > 600000) {
    lastNtp = millis();
    initTime();
  }
}