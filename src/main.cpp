#include <Arduino.h>
#include <WiFi.h>
#include <WiFiManager.h>
#include <PubSubClient.h>
#include <Preferences.h>

const int ledPin = 2;

char mqttServer[40] = "192.168.0.2";
const char* mqttTopic = "studio/onair";
bool shouldSaveConfig = false;

Preferences preferences;
WiFiClient espClient;
PubSubClient mqttClient(espClient);

void saveConfigCallback() {
  shouldSaveConfig = true;
}

void processMqttMessage(String message) {
  if (message == "ON") {
    digitalWrite(ledPin, HIGH);
  } else if (message == "OFF") {
    digitalWrite(ledPin, LOW);
  }
}

void mqttCallback(char* topic, byte* payload, unsigned int length) {
  String message;
  for (unsigned int i = 0; i < length; i++) {
    message += (char)payload[i];
  }
  
  Serial.print("Message arrived [");
  Serial.print(topic);
  Serial.print("]: ");
  Serial.println(message);

  processMqttMessage(message);
}

void handleMqttConnection() {
  while (!mqttClient.connected()) {
    Serial.print("Attempting MQTT connection to ");
    Serial.print(mqttServer);
    Serial.print("... ");
    
    String clientId = "OnAir-ESP32-";
    clientId += String(random(0xffff), HEX);

    if (mqttClient.connect(clientId.c_str())) {
      Serial.println("connected");
      mqttClient.subscribe(mqttTopic);
    } else {
      Serial.print("failed, rc=");
      Serial.print(mqttClient.state());
      Serial.println(" try again in 5 seconds");
      delay(5000);
    }
  }
}

void loadConfig() {
  preferences.begin("onairApp", false);
  String savedMqtt = preferences.getString("mqttIp", "");
  if (savedMqtt.length() > 0) {
    strlcpy(mqttServer, savedMqtt.c_str(), sizeof(mqttServer));
  }
}

void saveConfigIfNeeded() {
  if (shouldSaveConfig) {
    preferences.putString("mqttIp", mqttServer);
    Serial.println("Saved new MQTT broker IP to NVS");
  }
}

void setupWiFiAndPortal() {
  WiFiManager wifiManager;
  wifiManager.setSaveConfigCallback(saveConfigCallback);

  WiFiManagerParameter customMqttServer("server", "MQTT Broker IP", mqttServer, 40);
  wifiManager.addParameter(&customMqttServer);

  Serial.println("Connecting to WiFi or starting portal...");
  
  if (!wifiManager.autoConnect("OnAir_Setup")) {
    Serial.println("Failed to connect and hit timeout");
    delay(3000);
    ESP.restart();
  }

  Serial.println("Connected to WiFi");
  strlcpy(mqttServer, customMqttServer.getValue(), sizeof(mqttServer));
}

void setupMqtt() {
  mqttClient.setServer(mqttServer, 1883);
  mqttClient.setCallback(mqttCallback);
}

void setup() {
  Serial.begin(115200);
  pinMode(ledPin, OUTPUT);
  digitalWrite(ledPin, LOW);

  loadConfig();
  setupWiFiAndPortal();
  saveConfigIfNeeded();
  setupMqtt();
}

void loop() {
  if (!mqttClient.connected()) {
    handleMqttConnection();
  }
  mqttClient.loop();
}