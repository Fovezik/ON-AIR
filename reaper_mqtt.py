import paho.mqtt.client as mqtt

class OnAirSystem:
    def __init__(self, brokerAddress="127.0.0.1", port=1883, topic="studio/onair"):
        self.brokerAddress = brokerAddress
        self.port = port
        self.topic = topic
        self.lastRecordState = -1
        self.mqttClient = mqtt.Client(mqtt.CallbackAPIVersion.VERSION2)

    def connectToBroker(self):
        try:
            self.mqttClient.connect(self.brokerAddress, self.port, 60)
            self.mqttClient.loop_start() 
            RPR_ShowConsoleMsg("OnAirSystem: Connected to MQTT broker!\n")
            return True
        except Exception as e:
            RPR_ShowConsoleMsg("OnAirSystem ERROR: Cannot connect to broker: " + str(e) + "\n")
            return False

    def checkRecordState(self):
        currentState = RPR_GetToggleCommandState(1013) 

        if currentState != self.lastRecordState:
            self.lastRecordState = currentState
            
            if currentState == 1:
                self.mqttClient.publish(self.topic, "ON")
                RPR_ShowConsoleMsg("OnAir: ON\n")
            else:
                self.mqttClient.publish(self.topic, "OFF")
                RPR_ShowConsoleMsg("OnAir: OFF\n")
        
        RPR_defer("onAirApp.checkRecordState()")


if __name__ == "__main__":
    onAirApp = OnAirSystem(
        brokerAddress="127.0.0.1", 
        port=1883, 
        topic="studio/onair"
    )
    
    if onAirApp.connectToBroker():
        onAirApp.checkRecordState()