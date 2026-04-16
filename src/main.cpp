#include <Arduino.h>
#include <WiFiManager.h>
#include <PubSubClient.h>

#define LED 2

void setup() {
  pinMode(LED, OUTPUT);
  Serial.begin(115200);
  Serial.println("ESP32 gotowy! Rozpoczynam mruganie...");
}

void loop() {
  digitalWrite(LED, HIGH);
  Serial.println("Dioda: ON");
  delay(500);
  digitalWrite(LED, LOW);
  Serial.println("Dioda: OFF");
  delay(500);
}