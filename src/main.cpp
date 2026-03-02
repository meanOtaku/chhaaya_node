#include <Arduino.h>
#include <WiFi.h>
#include <AsyncTCP.h>
#include <ESPAsyncWebServer.h>
#include <AsyncMqttClient.h>
#include <ArduinoJson.h>
#include "LittleFS.h"

/* ===================== CONFIG ===================== */

#define MQTT_PORT 1883        // TCP MQTT
#define AP_SSID "ESP-WIFI-MANAGER"
#define LED_PIN 2

/* ===================== OBJECTS ===================== */

AsyncWebServer server(80);
AsyncMqttClient mqttClient;
StaticJsonDocument<256> jsonDoc;

/* ===================== GLOBALS ===================== */

String ssid, pass, ip, gateway;
String mqtt_ip;

String deviceId;
String statusTopic;
String controlTopic;

IPAddress localIP, localGateway;
IPAddress subnet(255, 255, 0, 0);

unsigned long lastStatus = 0;

/* ===================== FILE PATHS ===================== */

const char* ssidPath    = "/ssid.txt";
const char* passPath    = "/pass.txt";
const char* ipPath      = "/ip.txt";
const char* gatewayPath = "/gateway.txt";
const char* mqttIPPath  = "/mqtt_ip.txt";

/* ===================== FILESYSTEM ===================== */

String readFile(const char* path) {
  if (!LittleFS.exists(path)) return "";
  File f = LittleFS.open(path);
  String v = f.readStringUntil('\n');
  f.close();
  return v;
}

void writeFile(const char* path, const char* data) {
  File f = LittleFS.open(path, FILE_WRITE);
  f.print(data);
  f.close();
}

/* ===================== STATUS JSON ===================== */

void publishStatus(const char* state) {
  jsonDoc.clear();
  jsonDoc["device"] = deviceId;
  jsonDoc["state"]  = state;
  jsonDoc["ip"]     = WiFi.localIP().toString();
  jsonDoc["rssi"]   = WiFi.RSSI();
  jsonDoc["uptime"] = millis() / 1000;

  char buffer[256];
  serializeJson(jsonDoc, buffer);

  mqttClient.publish(
    statusTopic.c_str(),
    1,
    true,
    buffer
  );
}

/* ===================== MQTT CALLBACKS ===================== */

void onMqttConnect(bool) {
  Serial.println("✅ MQTT CONNECTED");
  publishStatus("online");
  mqttClient.subscribe(controlTopic.c_str(), 0);
}

void onMqttDisconnect(AsyncMqttClientDisconnectReason) {
  Serial.println("❌ MQTT DISCONNECTED");
}

void onMqttMessage(
  char* topic,
  char* payload,
  AsyncMqttClientMessageProperties,
  size_t len,
  size_t,
  size_t
) {
  String msg;
  for (size_t i = 0; i < len; i++) msg += payload[i];

  if (String(topic) == controlTopic) {
    if (msg == "on")  digitalWrite(LED_PIN, HIGH);
    if (msg == "off") digitalWrite(LED_PIN, LOW);
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

  Serial.println("================================");
  Serial.println("ESP32 CONNECTED (STA MODE)");
  Serial.print("IP: ");
  Serial.println(WiFi.localIP());
  Serial.println("================================");

  return true;
}

/* ===================== SETUP ===================== */

void setup() {
  Serial.begin(115200);
  pinMode(LED_PIN, OUTPUT);

  LittleFS.begin(true);

  ssid    = readFile(ssidPath);
  pass    = readFile(passPath);
  ip      = readFile(ipPath);
  gateway = readFile(gatewayPath);
  mqtt_ip = readFile(mqttIPPath);

  deviceId = String((uint32_t)ESP.getEfuseMac(), HEX);
  statusTopic  = "devices/" + deviceId + "/status";
  controlTopic = "devices/" + deviceId + "/control";

  if (connectWiFi()) {

    server.serveStatic("/", LittleFS, "/");
    server.begin();

    mqttClient.onConnect(onMqttConnect);
    mqttClient.onDisconnect(onMqttDisconnect);
    mqttClient.onMessage(onMqttMessage);

    mqttClient.setServer(mqtt_ip.c_str(), MQTT_PORT);

    // LAST WILL (works on TCP)
    mqttClient.setWill(
      statusTopic.c_str(),
      1,
      true,
      "{\"state\":\"offline\"}"
    );

    mqttClient.connect();

    Serial.print("MQTT Broker: ");
    Serial.print(mqtt_ip);
    Serial.println(":1883");

  } else {

    WiFi.softAP(AP_SSID);
    IPAddress apIP = WiFi.softAPIP();

    Serial.println("================================");
    Serial.println("ESP32 AP MODE");
    Serial.print("SSID: ");
    Serial.println(AP_SSID);
    Serial.print("Config URL: http://");
    Serial.println(apIP);
    Serial.println("================================");

    server.begin();
  }
}

/* ===================== LOOP ===================== */

void loop() {
  if (WiFi.isConnected() && millis() - lastStatus > 30000) {
    lastStatus = millis();
    publishStatus("online");
  }
}