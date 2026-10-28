#include <Arduino.h>
#include <Preferences.h>
#include <PubSubClient.h>
#include <WiFi.h>
#include <WiFiManager.h>
#include <esp_system.h>

#define DEBUG_ONAIR 1

#if DEBUG_ONAIR
#define DEBUG_PRINT(x) Serial.print(x)
#define DEBUG_PRINTLN(x) Serial.println(x)
#else
#define DEBUG_PRINT(x)
#define DEBUG_PRINTLN(x)
#endif

// ==========================================
// Hardware Configuration
// ==========================================
constexpr int PIN_RELAY = 4;
constexpr int PIN_STATUS_LED = 21;
constexpr int PIN_ACTIVE_LED = 18;
constexpr int PIN_PASSTHROUGH_SWITCH = 19;
constexpr int PIN_FACTORY_RESET = 22;

// Relay output logic:
// Set to HIGH if relay energizes on HIGH (e.g. transistor switch).
// Set to LOW if relay energizes on LOW (typical for optocoupled relay modules).
constexpr uint8_t RELAY_ACTIVE_LEVEL = HIGH;
constexpr uint8_t RELAY_INACTIVE_LEVEL = (RELAY_ACTIVE_LEVEL == HIGH) ? LOW : HIGH;

// ==========================================
// LED Indicator States
// ==========================================
enum class LedState {
  OFF,
  SOLID_ON,      // Connected to WiFi and MQTT broker
  FAST_BLINK,    // Connecting to WiFi or MQTT broker (200ms)
  SLOW_BLINK,    // WiFi configuration portal active (1000ms)
  STROBE_BLINK   // Factory reset hold warning (60ms)
};

class OnAirController;
extern OnAirController onAirSystem;

// C-style callback wrappers for external libraries
void globalSaveParamsCallback();
void globalSaveConfigCallback();
void globalConfigModeCallback(WiFiManager *myWiFiManager);
void globalConfigPortalTimeoutCallback();
void globalMqttCallback(char *topic, byte *payload, unsigned int length);

/**
 * @class OnAirController
 * @brief Fully encapsulated controller managing WiFi, MQTT, hardware overrides,
 * non-blocking debouncing, and system state.
 */
class OnAirController {
private:
  // Network configuration buffers
  char mqttServer[40] = "";
  char mqttPort[6] = "1883";
  char mqttTopic[64] = "studio/onair";

  // System state flags
  bool lastMqttCommandState = false;
  bool isPassthroughActive = false;
  bool isNetworkSetupActive = false;
  bool isFactoryResetWarning = false;

  // Timers
  unsigned long lastMqttRetryTime = 0;
  unsigned long lastWiFiRetryTime = 0;
  unsigned long lastLedToggleTime = 0;
  bool ledCurrentLevel = false;

  // Core objects
  Preferences preferences;
  WiFiClient espClient;
  PubSubClient mqttClient;

  WiFiManager wifiManager;
  WiFiManagerParameter customMqttServer;
  WiFiManagerParameter customMqttPort;
  WiFiManagerParameter customMqttTopic;

public:
  OnAirController()
      : mqttClient(espClient),
        customMqttServer("server", "MQTT Broker IP", mqttServer, sizeof(mqttServer)),
        customMqttPort("port", "MQTT Broker Port", mqttPort, sizeof(mqttPort)),
        customMqttTopic("topic", "MQTT Topic", mqttTopic, sizeof(mqttTopic)) {
    wifiManager.addParameter(&customMqttServer);
    wifiManager.addParameter(&customMqttPort);
    wifiManager.addParameter(&customMqttTopic);
  }

  /**
   * @brief Initializes GPIO pins and initial states.
   */
  void initializeHardware() {
    pinMode(PIN_RELAY, OUTPUT);
    pinMode(PIN_STATUS_LED, OUTPUT);
    pinMode(PIN_ACTIVE_LED, OUTPUT);
    pinMode(PIN_PASSTHROUGH_SWITCH, INPUT_PULLUP);
    pinMode(PIN_FACTORY_RESET, INPUT_PULLUP);

    digitalWrite(PIN_RELAY, RELAY_INACTIVE_LEVEL);
    digitalWrite(PIN_STATUS_LED, LOW);
    digitalWrite(PIN_ACTIVE_LED, LOW);

    // Initial check of physical passthrough switch
    isPassthroughActive = (digitalRead(PIN_PASSTHROUGH_SWITCH) == LOW);
    DEBUG_PRINTLN(F("[HW] Hardware pins initialized."));
  }

  /**
   * @brief Loads MQTT Broker configuration from Non-Volatile Storage (NVS).
   */
  void loadConfig() {
    preferences.begin("onAirApp", false);
    String savedServer = preferences.getString("mqttIp", "");
    String savedPort = preferences.getString("mqttPort", "1883");
    String savedTopic = preferences.getString("mqttTopic", "studio/onair");
    preferences.end();

    if (savedServer.length() > 0) {
      strlcpy(mqttServer, savedServer.c_str(), sizeof(mqttServer));
      DEBUG_PRINT(F("[CONFIG] Loaded MQTT IP: "));
      DEBUG_PRINTLN(mqttServer);
    } else {
      DEBUG_PRINTLN(F("[CONFIG] No MQTT IP saved."));
    }

    if (savedPort.length() > 0) {
      strlcpy(mqttPort, savedPort.c_str(), sizeof(mqttPort));
    }
    if (savedTopic.length() > 0) {
      strlcpy(mqttTopic, savedTopic.c_str(), sizeof(mqttTopic));
    }

    customMqttServer.setValue(mqttServer, sizeof(mqttServer));
    customMqttPort.setValue(mqttPort, sizeof(mqttPort));
    customMqttTopic.setValue(mqttTopic, sizeof(mqttTopic));
  }

  /**
   * @brief Reads custom parameters from WiFiManager and commits them to NVS.
   */
  void saveCustomParams() {
    strlcpy(mqttServer, customMqttServer.getValue(), sizeof(mqttServer));
    strlcpy(mqttPort, customMqttPort.getValue(), sizeof(mqttPort));
    strlcpy(mqttTopic, customMqttTopic.getValue(), sizeof(mqttTopic));

    preferences.begin("onAirApp", false);
    preferences.putString("mqttIp", mqttServer);
    preferences.putString("mqttPort", mqttPort);
    preferences.putString("mqttTopic", mqttTopic);
    preferences.end();

    DEBUG_PRINTLN(F("[CONFIG] Custom parameters saved to NVS:"));
    DEBUG_PRINT(F("  Broker: "));
    DEBUG_PRINTLN(mqttServer);
    DEBUG_PRINT(F("  Port:   "));
    DEBUG_PRINTLN(mqttPort);
    DEBUG_PRINT(F("  Topic:  "));
    DEBUG_PRINTLN(mqttTopic);
  }

  /**
   * @brief Initializes WiFiManager with non-blocking AP and portal timeout.
   */
  void setupWiFiAndPortal() {
    DEBUG_PRINTLN(F("[WIFI] Initializing WiFi/Portal..."));
    WiFi.mode(WIFI_STA);
    WiFi.setAutoReconnect(true);

    wifiManager.setSaveParamsCallback(globalSaveParamsCallback);
    wifiManager.setSaveConfigCallback(globalSaveConfigCallback);
    wifiManager.setAPCallback(globalConfigModeCallback);
    wifiManager.setConfigPortalTimeoutCallback(globalConfigPortalTimeoutCallback);
    wifiManager.setConfigPortalBlocking(false);
    wifiManager.setConfigPortalTimeout(120); // 2-minute timeout for configuration portal
    wifiManager.setAPClientCheck(true);      // Keep portal alive while a client is connected to AP
    wifiManager.setWebPortalClientCheck(true); // Reset timeout on web page activity

    if (wifiManager.autoConnect("OnAir_Setup")) {
      DEBUG_PRINTLN(F("[WIFI] AutoConnect success."));
      isNetworkSetupActive = false;
      saveCustomParams();
    } else {
      DEBUG_PRINTLN(F("[WIFI] Starting AP Portal (OnAir_Setup)..."));
      isNetworkSetupActive = true;
    }
  }

  /**
   * @brief Configures MQTT client server and callback.
   */
  void setupMqtt() {
    if (isNetworkSetupActive || strlen(mqttServer) == 0) {
      return;
    }

    uint16_t port = (uint16_t)atoi(mqttPort);
    if (port == 0) {
      port = 1883;
    }

    mqttClient.setServer(mqttServer, port);
    mqttClient.setCallback(globalMqttCallback);
    DEBUG_PRINTLN(F("[MQTT] Client configured."));
  }

  /**
   * @brief Non-blocking connection attempt to MQTT broker.
   */
  void handleMqttConnectionNonBlocking() {
    if (isNetworkSetupActive || isPassthroughActive || WiFi.status() != WL_CONNECTED) {
      return;
    }

    if (strlen(mqttServer) == 0) {
      DEBUG_PRINTLN(F("[MQTT] Broker IP not set, skipping connection."));
      return;
    }

    uint16_t port = (uint16_t)atoi(mqttPort);
    if (port == 0) port = 1883;
    mqttClient.setServer(mqttServer, port);
    mqttClient.setCallback(globalMqttCallback);

    DEBUG_PRINT(F("[MQTT] Attempting connection to "));
    DEBUG_PRINT(mqttServer);
    DEBUG_PRINT(F(":"));
    DEBUG_PRINTLN(port);

    // Persistent client ID based on ESP32 MAC address
    char clientId[32];
    uint64_t mac = ESP.getEfuseMac();
    snprintf(clientId, sizeof(clientId), "OnAir-%04X%08X", (uint16_t)(mac >> 32), (uint32_t)mac);

    if (mqttClient.connect(clientId)) {
      DEBUG_PRINTLN(F("[MQTT] Connected to broker."));
      DEBUG_PRINT(F("[MQTT] Subscribing to: "));
      DEBUG_PRINTLN(mqttTopic);
      mqttClient.subscribe(mqttTopic);
    } else {
      DEBUG_PRINT(F("[MQTT] Connect failed, rc="));
      DEBUG_PRINTLN(mqttClient.state());
    }
  }

  /**
   * @brief Shuts down network stack when Passthrough mode is active.
   */
  void disconnectAndSleepNetwork() {
    DEBUG_PRINTLN(F("[NET] Shutting down network services..."));
    if (isNetworkSetupActive) {
      wifiManager.stopConfigPortal();
      isNetworkSetupActive = false;
    }
    if (mqttClient.connected()) {
      mqttClient.disconnect();
    }
    WiFi.disconnect(true, false);
    WiFi.mode(WIFI_OFF);
    DEBUG_PRINTLN(F("[NET] Network offline."));
  }

  /**
   * @brief Updates the relay and indicator LEDs based on Passthrough or MQTT command.
   */
  void updateOutputs() {
    bool finalOutputState = isPassthroughActive || lastMqttCommandState;

    DEBUG_PRINT(F("[OUTPUT] Passthrough: "));
    DEBUG_PRINT(isPassthroughActive);
    DEBUG_PRINT(F(" | MQTT: "));
    DEBUG_PRINT(lastMqttCommandState);
    DEBUG_PRINT(F(" | Final: "));
    DEBUG_PRINTLN(finalOutputState);

    digitalWrite(PIN_RELAY, finalOutputState ? RELAY_ACTIVE_LEVEL : RELAY_INACTIVE_LEVEL);
    digitalWrite(PIN_ACTIVE_LED, finalOutputState ? HIGH : LOW);
  }

  /**
   * @brief Sets MQTT state received from broker callback.
   */
  void setMqttCommandState(bool state) {
    DEBUG_PRINT(F("[MQTT] State set to: "));
    DEBUG_PRINTLN(state ? "ON" : "OFF");
    lastMqttCommandState = state;
    updateOutputs();
  }

  void onConfigModeStarted() {
    DEBUG_PRINTLN(F("[WIFI] Entered configuration mode."));
    isNetworkSetupActive = true;
  }

  void onConfigPortalTimeout() {
    DEBUG_PRINTLN(F("\n[WIFI] Config portal timed out! Rebooting in 500ms..."));
    digitalWrite(PIN_STATUS_LED, LOW);
    delay(500);
    esp_restart();
  }

  /**
   * @brief Checks the physical Passthrough switch with 50ms software debounce.
   */
  void checkPassthroughSwitch() {
    bool reading = (digitalRead(PIN_PASSTHROUGH_SWITCH) == LOW);
    static bool lastDebounceReading = false;
    static unsigned long lastDebounceTime = 0;

    if (reading != lastDebounceReading) {
      lastDebounceTime = millis();
      lastDebounceReading = reading;
    }

    if ((millis() - lastDebounceTime) > 50) {
      if (reading != isPassthroughActive) {
        isPassthroughActive = reading;
        updateOutputs();

        if (isPassthroughActive) {
          DEBUG_PRINTLN(F("[SYS] Mode changed -> PASSTHROUGH (Manual Override ON)"));
          disconnectAndSleepNetwork();
        } else {
          DEBUG_PRINTLN(F("[SYS] Mode changed -> NORMAL"));
          setupWiFiAndPortal();
          setupMqtt();
        }
      }
    }
  }

  /**
   * @brief Monitors the physical factory reset button with debounce filtering.
   * Holding >= 3s starts strobe LED warning.
   * Holding >= 5s executes immediate wipe & restart.
   */
  void monitorFactoryReset() {
    bool rawReading = (digitalRead(PIN_FACTORY_RESET) == LOW);
    static bool debouncedPressed = false;
    static bool lastRawReading = false;
    static unsigned long lastDebounceTime = 0;
    static unsigned long resetPressStartTime = 0;

    // 50ms software debounce filter to eliminate contact chatter
    if (rawReading != lastRawReading) {
      lastDebounceTime = millis();
      lastRawReading = rawReading;
    }

    if ((millis() - lastDebounceTime) > 50) {
      if (rawReading != debouncedPressed) {
        debouncedPressed = rawReading;
        if (debouncedPressed) {
          resetPressStartTime = millis();
          DEBUG_PRINTLN(F("[SYS] Reset button PRESSED."));
        } else {
          isFactoryResetWarning = false;
          DEBUG_PRINTLN(F("[SYS] Reset button released."));
        }
      }
    }

    if (debouncedPressed) {
      unsigned long holdDuration = millis() - resetPressStartTime;

      // Visual warning after 3 seconds of hold
      if (holdDuration >= 3000 && !isFactoryResetWarning) {
        isFactoryResetWarning = true;
        DEBUG_PRINTLN(F("[SYS] Hold 2s more to execute Factory Reset..."));
      }

      // Trigger reset when held for 5 seconds
      if (holdDuration >= 5000) {
        DEBUG_PRINTLN(F("!!! [SYS] FACTORY RESET TRIGGERED !!!"));
        digitalWrite(PIN_STATUS_LED, HIGH);

        if (isNetworkSetupActive) {
          wifiManager.stopConfigPortal();
        }

        wifiManager.resetSettings();
        preferences.begin("onAirApp", false);
        preferences.clear();
        preferences.end();

        DEBUG_PRINTLN(F("[SYS] Preferences wiped. Rebooting in 500ms..."));
        delay(500);
        esp_restart();
      }
    }
  }

  /**
   * @brief Non-blocking status LED renderer without FreeRTOS timer dependencies.
   */
  void updateStatusLed() {
    if (isPassthroughActive) {
      digitalWrite(PIN_STATUS_LED, LOW);
      return;
    }

    LedState targetState;
    if (isFactoryResetWarning) {
      targetState = LedState::STROBE_BLINK;
    } else if (isNetworkSetupActive) {
      targetState = LedState::SLOW_BLINK;
    } else if (WiFi.status() != WL_CONNECTED || !mqttClient.connected()) {
      targetState = LedState::FAST_BLINK;
    } else {
      targetState = LedState::SOLID_ON;
    }

    unsigned long now = millis();
    switch (targetState) {
      case LedState::OFF:
        digitalWrite(PIN_STATUS_LED, LOW);
        break;

      case LedState::SOLID_ON:
        digitalWrite(PIN_STATUS_LED, HIGH);
        break;

      case LedState::FAST_BLINK:
        if (now - lastLedToggleTime >= 200) {
          lastLedToggleTime = now;
          ledCurrentLevel = !ledCurrentLevel;
          digitalWrite(PIN_STATUS_LED, ledCurrentLevel ? HIGH : LOW);
        }
        break;

      case LedState::SLOW_BLINK:
        if (now - lastLedToggleTime >= 1000) {
          lastLedToggleTime = now;
          ledCurrentLevel = !ledCurrentLevel;
          digitalWrite(PIN_STATUS_LED, ledCurrentLevel ? HIGH : LOW);
        }
        break;

      case LedState::STROBE_BLINK:
        if (now - lastLedToggleTime >= 60) {
          lastLedToggleTime = now;
          ledCurrentLevel = !ledCurrentLevel;
          digitalWrite(PIN_STATUS_LED, ledCurrentLevel ? HIGH : LOW);
        }
        break;
    }
  }

  /**
   * @brief Main loop worker.
   */
  void runLoop() {
    // 1. Hardware checks
    monitorFactoryReset();
    checkPassthroughSwitch();
    updateStatusLed();

    // 2. WiFi portal processing if active
    if (isNetworkSetupActive) {
      wifiManager.process();
      if (WiFi.status() == WL_CONNECTED) {
        DEBUG_PRINTLN(F("[WIFI] Portal connected! Initializing MQTT..."));
        isNetworkSetupActive = false;
        saveCustomParams();
        setupMqtt();
      } else if (!wifiManager.getConfigPortalActive()) {
        DEBUG_PRINTLN(F("[WIFI] Portal timed out / inactive! Rebooting in 500ms..."));
        delay(500);
        esp_restart();
      }
      return;
    }

    // 3. Normal mode network maintenance
    if (!isPassthroughActive) {
      unsigned long now = millis();

      if (WiFi.status() != WL_CONNECTED) {
        // WiFi dropped - attempt reconnection every 10 seconds
        if (now - lastWiFiRetryTime > 10000) {
          lastWiFiRetryTime = now;
          DEBUG_PRINTLN(F("[WIFI] Connection lost. Reconnecting..."));
          WiFi.reconnect();
        }
      } else {
        // WiFi connected - maintain MQTT client
        if (strlen(mqttServer) > 0) {
          if (!mqttClient.connected()) {
            if (now - lastMqttRetryTime > 5000) {
              lastMqttRetryTime = now;
              handleMqttConnectionNonBlocking();
            }
          } else {
            mqttClient.loop();
          }
        }
      }
    }
  }
};

OnAirController onAirSystem;

// Callback implementations
void globalSaveParamsCallback() {
  onAirSystem.saveCustomParams();
}

void globalSaveConfigCallback() {
  onAirSystem.saveCustomParams();
}

void globalConfigModeCallback(WiFiManager *myWiFiManager) {
  onAirSystem.onConfigModeStarted();
}

void globalConfigPortalTimeoutCallback() {
  onAirSystem.onConfigPortalTimeout();
}

void globalMqttCallback(char *topic, byte *payload, unsigned int length) {
  // Trim leading whitespace/newlines
  while (length > 0 && (payload[0] == ' ' || payload[0] == '\r' || payload[0] == '\n')) {
    payload++;
    length--;
  }
  // Trim trailing whitespace/newlines
  while (length > 0 && (payload[length - 1] == ' ' || payload[length - 1] == '\r' || payload[length - 1] == '\n')) {
    length--;
  }

  // Efficient in-place comparison without dynamic string allocation
  if ((length == 2 && strncasecmp((char *)payload, "ON", 2) == 0) ||
      (length == 1 && payload[0] == '1')) {
    onAirSystem.setMqttCommandState(true);
  } else if ((length == 3 && strncasecmp((char *)payload, "OFF", 3) == 0) ||
             (length == 1 && payload[0] == '0')) {
    onAirSystem.setMqttCommandState(false);
  } else {
    DEBUG_PRINT(F("[MQTT] Unknown command payload received, len="));
    DEBUG_PRINTLN(length);
  }
}

void setup() {
  Serial.begin(115200);
  delay(500);
  DEBUG_PRINTLN(F("\n--- SYSTEM BOOT ---"));

  onAirSystem.initializeHardware();
  onAirSystem.loadConfig();
  onAirSystem.updateOutputs();

  // Determine initial boot target
  bool isPassthrough = (digitalRead(PIN_PASSTHROUGH_SWITCH) == LOW);
  if (isPassthrough) {
    DEBUG_PRINTLN(F("[BOOT] Mode: PASSTHROUGH"));
    WiFi.mode(WIFI_OFF);
  } else {
    DEBUG_PRINTLN(F("[BOOT] Mode: NORMAL"));
    onAirSystem.setupWiFiAndPortal();
    onAirSystem.setupMqtt();
  }
}

void loop() {
  onAirSystem.runLoop();
}