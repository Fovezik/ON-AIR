import paho.mqtt.client as mqtt
import json
import os

# Configuration file location (defined at module level)
CONFIG_FILE = os.path.join(RPR_GetResourcePath(), "reaper_mqtt_config.json")

# Global app instance
onAirApp = None

class OnAirSystem:
    def __init__(self, brokerAddress, port, topic):
        self.brokerAddress = brokerAddress
        self.port = port
        self.topic = topic
        self.lastRecordState = -1
        self.mqttClient = mqtt.Client(mqtt.CallbackAPIVersion.VERSION2)
        self.connected = False

    def connectToBroker(self):
        try:
            self.mqttClient.connect(self.brokerAddress, self.port, 60)
            self.mqttClient.loop_start() 
            RPR_ShowConsoleMsg("OnAirSystem: Connected to MQTT broker!\n")
            self.connected = True
            return True
        except Exception as e:
            RPR_ShowConsoleMsg("OnAirSystem ERROR: Cannot connect to broker: " + str(e) + "\n")
            self.connected = False
            return False

    def checkRecordState(self):
        if not self.connected:
            return
            
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

def load_config():
    """Load configuration from file or return defaults"""
    if os.path.exists(CONFIG_FILE):
        try:
            with open(CONFIG_FILE, 'r') as f:
                return json.load(f)
        except:
            return {"broker": "", "port": "1883", "topic": ""}
    return {"broker": "", "port": "1883", "topic": ""}

def save_config(config):
    """Save configuration to file"""
    try:
        with open(CONFIG_FILE, 'w') as f:
            json.dump(config, f, indent=2)
        return True
    except Exception as e:
        RPR_ShowConsoleMsg("Error saving config: " + str(e) + "\n")
        return False

def show_config_dialog():
    """Show a dialog to configure MQTT settings using gfx"""
    config = load_config()
    broker = config.get("broker", "")
    port = config.get("port", "1883")
    topic = config.get("topic", "")
    # RPR_GetUserInputs in the Python REAPER API expects a maximum-length
    # buffer as the 5th parameter; add 1024 to avoid missing-argument errors
    user_input = RPR_GetUserInputs("MQTT Configuration", 3,
                                   "Broker Address (IP or hostname):,Port:,Topic:",
                                   broker + "," + port + "," + topic,
                                   1024)

    # Debug: show raw return
    try:
        RPR_ShowConsoleMsg("DEBUG: RPR_GetUserInputs returned: %r\n" % (user_input,))
    except Exception:
        pass

    if user_input[0]:
        # Determine which element contains the input string (varies by API wrapper)
        if isinstance(user_input, (list, tuple)) and len(user_input) >= 5:
            input_str = user_input[4]
        elif isinstance(user_input, (list, tuple)) and len(user_input) >= 2:
            input_str = user_input[1]
        else:
            input_str = ""

        # Parse returned CSV robustly (support commas, tabs or semicolons)
        try:
            import csv
            reader = csv.reader([input_str])
            values = next(reader)
        except Exception:
            vals = input_str
            if '\t' in vals:
                values = vals.split('\t')
            elif ';' in vals:
                values = vals.split(';')
            else:
                values = vals.split(',')
        values = [v.strip() for v in values]
        RPR_ShowConsoleMsg("DEBUG: parsed values: %r\n" % (values,))
        if len(values) >= 3:
            # accept at least three values; ignore extras
            values = values[:3]
            new_config = {
                "broker": values[0],
                "port": values[1],
                "topic": values[2]
            }
            if new_config["broker"] and new_config["port"] and new_config["topic"]:
                # Return but don't save here; saving occurs after a successful connection
                RPR_ShowConsoleMsg("DEBUG: Got new config (not yet saved): %r\n" % (new_config,))
                return new_config
            else:
                RPR_MB("All fields are required!", "Validation Error", 0)
    
    return None

def initialize_app():
    """Initialize the application with configuration"""
    global onAirApp
    # Keep prompting until a valid configuration is provided and connection succeeds,
    # or the user cancels the dialog.
    config = load_config()

    while True:
        if not config.get("broker") or not config.get("port") or not config.get("topic"):
            RPR_ShowConsoleMsg("MQTT Configuration missing. Opening configuration dialog...\n")
            new_config = show_config_dialog()
            if not new_config:
                RPR_ShowConsoleMsg("Configuration cancelled. Will retry on next startup.\n")
                return False
            config = new_config

        try:
            port = int(config.get("port", 1883))
        except:
            port = 1883

        candidate = OnAirSystem(
            brokerAddress=config.get("broker"),
            port=port,
            topic=config.get("topic")
        )

        if candidate.connectToBroker():
            # Save working config and start background loop
            save_config(config)
            onAirApp = candidate
            RPR_ShowConsoleMsg("OnAir system started successfully in background.\n")
            onAirApp.checkRecordState()
            return True
        else:
            # Connection failed - allow user to re-enter settings
            retry = RPR_MB("Failed to connect to MQTT broker. Retry configuration?", "Connection Error", 1)
            # RPR_MB returns 1 for OK/Yes, 2 for Cancel/No depending on build; treat falsy as cancel
            if not retry:
                return False
            RPR_ShowConsoleMsg("Retrying configuration...\n")
            new_config = show_config_dialog()
            if not new_config:
                RPR_ShowConsoleMsg("Configuration cancelled. Will retry on next startup.\n")
                return False
            config = new_config

if __name__ == "__main__":
    initialize_app()