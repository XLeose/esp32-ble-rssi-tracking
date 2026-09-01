import os
import time
import json
import threading
import paho.mqtt.client as mqtt
from paho.mqtt.enums import CallbackAPIVersion

try:
    from dotenv import load_dotenv
    load_dotenv()
except ImportError:
    pass

# MQTT Broker Configuration (Loaded from environment variables or default fallbacks)
BROKER_IP = os.getenv("MQTT_BROKER_IP", "mosquitto")
BROKER_PORT = int(os.getenv("MQTT_BROKER_PORT", "1883"))
BROKER_USER = os.getenv("MQTT_USERNAME", "YOUR_MQTT_USERNAME")
BROKER_PASS = os.getenv("MQTT_PASSWORD", "YOUR_MQTT_PASSWORD")
LISTEN_TOPIC = os.getenv("MQTT_LISTEN_TOPIC", "arge/test_device")
PUBLISH_TOPIC = os.getenv("MQTT_PUBLISH_TOPIC", "arge/decision")
CLIENT_ID = os.getenv("MQTT_CLIENT_ID", "Backend_Decision_Motor")
DECISION_INTERVAL_SEC = float(os.getenv("DECISION_INTERVAL_SEC", "3.0"))

device_info = {}
last_published_state = {}

def on_connect(client, userdata, flags, reason_code, properties=None):
    """Callback triggered when the client connects to the MQTT broker."""
    print(f"[{time.strftime('%X')}] Connected to MQTT Broker. Subscribing to: {LISTEN_TOPIC}")
    client.subscribe(LISTEN_TOPIC)

def on_message(client, userdata, msg):
    """Callback triggered when a message is received from a subscribed topic."""
    try:
        payload = json.loads(msg.payload.decode("utf-8"))
        tag_name = payload.get("tag")
        scanner_name = payload.get("scanner")
        rssi = payload.get("rssi")

        if tag_name and scanner_name is not None and rssi is not None:
            if tag_name not in device_info:
                device_info[tag_name] = {}
            device_info[tag_name][scanner_name] = rssi
            
    except Exception as e:
        print("Invalid data format received:", e)

def decision_loop(mqtt_client):
    """
    Periodic decision engine loop:
    Evaluates RSSI measurements from all scanners for each tag, determines
    the closest scanner (or equidistant range), and publishes location decisions.
    """
    while True:
        time.sleep(DECISION_INTERVAL_SEC)
        if not device_info:
            continue
            
        print("-" * 40)
        for tag, scanners in device_info.items():
            if not scanners:
                continue

            best_rssi = max(scanners.values())
            best_scanners = [s for s, rssi in scanners.items() if rssi == best_rssi]

            if len(best_scanners) > 1:
                closest_scanner = f"between {' and '.join(best_scanners)}"
            else:
                closest_scanner = best_scanners[0]
                
            last_state = last_published_state.get(tag)

            # Skip publishing if state hasn't changed
            if last_state == (closest_scanner, best_rssi):
                continue

            last_published_state[tag] = (closest_scanner, best_rssi)
            current_time = time.strftime('%X')
            print(f"[{current_time}] Decision -> Tag: {tag} is located at: {closest_scanner} (Signal: {best_rssi} dBm)")

            decision_payload = {
                "tag": tag,
                "location": closest_scanner,
                "rssi": best_rssi,
                "timestamp": current_time
            }

            mqtt_client.publish(
                PUBLISH_TOPIC, 
                payload=json.dumps(decision_payload), 
                qos=1, 
                retain=True
            )

if __name__ == "__main__":
    # Create MQTT client with a descriptive client ID
    client = mqtt.Client(CallbackAPIVersion.VERSION2, client_id=CLIENT_ID)
    client.on_connect = on_connect
    client.on_message = on_message

    print("Decision Engine Backend Starting...")

    # Set credentials if configured
    if BROKER_USER and BROKER_USER != "YOUR_MQTT_USERNAME":
        client.username_pw_set(BROKER_USER, BROKER_PASS)

    client.connect(BROKER_IP, BROKER_PORT, 60)

    # Start background decision loop thread
    motor_thread = threading.Thread(target=decision_loop, args=(client,), daemon=True)
    motor_thread.start()

    # Run network loop
    client.loop_forever()
