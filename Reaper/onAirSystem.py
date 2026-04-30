import os
import json
import paho.mqtt.client as mqtt

class OnAirSystem:
    """Manages MQTT connection and REAPER recording state synchronization."""

    def __init__(self):
        resourcePath = RPR_GetResourcePath()
        scriptDir = os.path.join(resourcePath, "Scripts")
        self.configFile = os.path.join(scriptDir, "onAirConfig.json")
        self.mqttClient = None
        self.lastRecordState = -1
        self.topic = ""

    def loadConfig(self):
        """Loads configuration from JSON file if it exists."""
        if os.path.exists(self.configFile):
            try:
                with open(self.configFile, 'r') as fileObject:
                    return json.load(fileObject)
            except ValueError:
                return None
        return None

    def saveConfig(self, configData):
        """Saves configuration to JSON file."""
        with open(self.configFile, 'w') as fileObject:
            json.dump(configData, fileObject, indent=4)

    def promptUserForConfig(self, defaultIp="192.168.0.3", defaultPort="1883", defaultTopic="studio/onair"):
        """Displays a REAPER dialog to gather MQTT settings."""
        dialogTitle = "OnAir MQTT Setup"
        captions = "Broker IP:,Port:,Topic:"
        defaultValues = f"{defaultIp},{defaultPort},{defaultTopic}"
        
        result = RPR_GetUserInputs(dialogTitle, 3, captions, defaultValues, 512)
        
        if result[0]:
            values = result[4].split(',')
            if len(values) == 3:
                return {
                    "brokerIp": values[0].strip(),
                    "port": values[1].strip(),
                    "topic": values[2].strip()
                }
        return None

    def connectToBroker(self, configData):
        """Attempts to connect to the MQTT broker."""
        try:
            self.topic = configData["topic"]
            self.mqttClient = mqtt.Client(mqtt.CallbackAPIVersion.VERSION2)
            self.mqttClient.connect(configData["brokerIp"], int(configData["port"]), 2)
            self.mqttClient.loop_start()
            return True
        except Exception:
            return False

    def checkRecordState(self):
        """Checks REAPER record state and publishes to MQTT on change."""
        currentState = RPR_GetToggleCommandState(1013)
        
        if currentState != self.lastRecordState:
            self.lastRecordState = currentState
            if currentState == 1:
                self.mqttClient.publish(self.topic, "ON")
            else:
                self.mqttClient.publish(self.topic, "OFF")
                
        RPR_defer("onAirApp.checkRecordState()")

    def run(self):
        """Main execution logic for configuration and connection handling."""
        configData = self.loadConfig()
        connected = False
        
        while not connected:
            if not configData:
                configData = self.promptUserForConfig()
                if not configData:
                    return
                self.saveConfig(configData)
            
            if self.connectToBroker(configData):
                connected = True
                self.checkRecordState()
            else:
                configData = self.promptUserForConfig(
                    configData.get("brokerIp", "192.168.0.3"),
                    configData.get("port", "1883"),
                    configData.get("topic", "studio/onair")
                )
                if not configData:
                    return
                self.saveConfig(configData)


if __name__ == "__main__":
    onAirApp = OnAirSystem()
    onAirApp.run()