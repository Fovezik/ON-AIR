#include <Arduino.h>
#include <WiFi.h>
#include <WiFiManager.h>
#include <PubSubClient.h>
#include <Preferences.h>
#include <Ticker.h>

class OnAirClient;
extern OnAirClient onAirApp;

/**
 * @brief Global callback wrapper to trigger the save configuration flag.
 */
void globalSaveConfigCallback();

/**
 * @brief Global callback wrapper to process incoming MQTT messages.
 * @param topic The MQTT topic the message was published to.
 * @param payload The raw message data.
 * @param length The length of the payload.
 */
void globalMqttCallback(char* topic, byte* payload, unsigned int length);

/**
 * @brief Global callback wrapper triggered when WiFiManager enters AP mode.
 * @param myWiFiManager Pointer to the WiFiManager instance.
 */
void globalConfigModeCallback(WiFiManager *myWiFiManager);

/**
 * @brief Global callback wrapper for Ticker to blink the status LED.
 */
void globalTickCallback();

/**
 * @class OnAirClient
 * @brief Manages WiFi, MQTT connection, relay state, and network status LED.
 */
class OnAirClient {
private:
    const int statusLedPin = 2;
    const int relayPin = 4;
    
    const int maxMqttRetries = 2;
    int currentMqttRetries = 0;
    char mqttServer[40] = "";
    const char* mqttTopic = "studio/onair";
    bool shouldSaveConfig = false;

    Preferences preferences;
    WiFiClient espClient;
    PubSubClient mqttClient;
    Ticker statusTicker;

    /**
     * @brief Resets WiFiManager settings, clears NVS preferences, and restarts the ESP32.
     */
    void resetDeviceAndCredentials() {
        WiFiManager wifiManager;
        wifiManager.resetSettings();
        
        preferences.begin("onAirApp", false);
        preferences.clear();
        preferences.end();
        
        ESP.restart();
    }

public:
    OnAirClient() : mqttClient(espClient) {}

    /**
     * @brief Sets the configuration save flag to true.
     */
    void markConfigForSaving() {
        shouldSaveConfig = true;
    }

    /**
     * @brief Toggles the current state of the status LED.
     */
    void toggleStatusLed() {
        int state = digitalRead(statusLedPin);
        digitalWrite(statusLedPin, !state);
    }

    /**
     * @brief Sets the status LED to blinking mode (Connecting).
     */
    void setStatusConnecting() {
        statusTicker.attach(0.3, globalTickCallback);
    }

    /**
     * @brief Sets the status LED to solid ON mode (Setup Portal).
     */
    void setStatusSetupMode() {
        statusTicker.detach();
        digitalWrite(statusLedPin, HIGH);
    }

    /**
     * @brief Sets the status LED to solid OFF mode (Connected).
     */
    void setStatusConnected() {
        statusTicker.detach();
        digitalWrite(statusLedPin, LOW);
    }

    /**
     * @brief Loads the saved MQTT broker IP from NVS memory.
     */
    void loadConfig() {
        preferences.begin("onAirApp", false);
        String savedMqtt = preferences.getString("mqttIp", "");
        if (savedMqtt.length() > 0) {
            strlcpy(mqttServer, savedMqtt.c_str(), sizeof(mqttServer));
        }
        preferences.end();
    }

    /**
     * @brief Saves the current MQTT broker IP to NVS memory if the flag is set.
     */
    void saveConfigIfNeeded() {
        if (shouldSaveConfig) {
            preferences.begin("onAirApp", false);
            preferences.putString("mqttIp", mqttServer);
            preferences.end();
            shouldSaveConfig = false;
        }
    }

    /**
     * @brief Controls the external relay based on the received MQTT message.
     * @param message The payload received from the MQTT broker.
     */
    void processMqttMessage(String message) {
        if (message == "ON") {
            digitalWrite(relayPin, HIGH);
        } else if (message == "OFF") {
            digitalWrite(relayPin, LOW);
        }
    }

    /**
     * @brief Attempts to connect to the MQTT broker, retrying up to a maximum limit before wiping credentials.
     */
    void handleMqttConnection() {
        setStatusConnecting();
        
        while (!mqttClient.connected()) {
            if (currentMqttRetries >= maxMqttRetries) {
                resetDeviceAndCredentials();
            }

            String clientId = "OnAir-ESP32-";
            clientId += String(random(0xffff), HEX);

            if (mqttClient.connect(clientId.c_str())) {
                currentMqttRetries = 0;
                mqttClient.subscribe(mqttTopic);
                setStatusConnected();
            } else {
                currentMqttRetries++;
                delay(5000);
            }
        }
    }

    /**
     * @brief Initializes WiFi connection or launches the configuration portal.
     */
    void setupWiFiAndPortal() {
        setStatusConnecting();
        
        WiFiManager wifiManager;
        wifiManager.setSaveConfigCallback(globalSaveConfigCallback);
        wifiManager.setAPCallback(globalConfigModeCallback);

        WiFiManagerParameter customMqttServer("server", "MQTT Broker IP", mqttServer, 40);
        wifiManager.addParameter(&customMqttServer);

        if (!wifiManager.autoConnect("OnAir_Setup")) {
            delay(3000);
            ESP.restart();
        }

        strlcpy(mqttServer, customMqttServer.getValue(), sizeof(mqttServer));
    }

    /**
     * @brief Configures the MQTT client settings.
     */
    void setupMqtt() {
        mqttClient.setServer(mqttServer, 1883);
        mqttClient.setCallback(globalMqttCallback);
    }

    /**
     * @brief Initializes hardware pins.
     */
    void initializeHardware() {
        pinMode(statusLedPin, OUTPUT);
        pinMode(relayPin, OUTPUT);
        digitalWrite(statusLedPin, LOW);
        digitalWrite(relayPin, LOW);
    }

    /**
     * @brief Maintains the MQTT connection and processes incoming messages.
     */
    void runLoop() {
        if (!mqttClient.connected()) {
            handleMqttConnection();
        }
        mqttClient.loop();
    }
};

OnAirClient onAirApp;

void globalSaveConfigCallback() {
    onAirApp.markConfigForSaving();
}

void globalMqttCallback(char* topic, byte* payload, unsigned int length) {
    String message;
    for (unsigned int i = 0; i < length; i++) {
        message += (char)payload[i];
    }
    onAirApp.processMqttMessage(message);
}

void globalConfigModeCallback(WiFiManager *myWiFiManager) {
    onAirApp.setStatusSetupMode();
}

void globalTickCallback() {
    onAirApp.toggleStatusLed();
}

void setup() {
    onAirApp.initializeHardware();
    onAirApp.loadConfig();
    onAirApp.setupWiFiAndPortal();
    onAirApp.saveConfigIfNeeded();
    onAirApp.setupMqtt();
}

void loop() {
    onAirApp.runLoop();
}