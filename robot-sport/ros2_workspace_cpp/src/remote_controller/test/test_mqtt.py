#!/usr/bin/env python3
"""MQTT load test for remote_controller.

Mirrors test.py but sends velocity commands over MQTT instead of WebSocket.
Publishes to the downlink topic and subscribes to both downlink/uplink topics
to observe publish echoes and node responses in the same run.

Requires: paho-mqtt (pip3 install paho-mqtt)
"""
import json
import threading
import time

import paho.mqtt.client as mqtt


# MQTT broker config (must match the node's config)
MQTT_BROKER = "localhost"
MQTT_PORT = 1883

# Topic config (must match the node's topic format)
REGION = "cn-sz"
TENANT_ID = "gs"
HUB_ID = "GS20250004"  # override to match your device SN

DOWNLINK_TOPIC = f"sys/{REGION}/{TENANT_ID}/{HUB_ID}/remote_control/downlink"
UPLINK_TOPIC = f"sys/{REGION}/{TENANT_ID}/{HUB_ID}/remote_control/uplink"

# Test parameters (same as test.py)
HZ = 30                          # Send frequency (Hz)
DURATION = 3                    # Test duration (seconds)
LINEAR_START = 0.1               # Starting linear velocity
ANGULAR_Z = 0.3                  # Fixed angular velocity
INCREMENT_PER_SECOND = 0.5       # Linear velocity increment per second

INCREMENT_PER_STEP = INCREMENT_PER_SECOND / HZ
TOTAL_STEPS = HZ * DURATION

# Shared state for TX/RX tracking
publish_count = 0
downlink_echo_count = 0
uplink_response_count = 0
last_downlink_echo = None
last_uplink_response = None
published_payload_index = {}
response_lock = threading.Lock()


def make_command_key(payload):
    if not isinstance(payload, dict):
        return None

    tracked_fields = {}
    for field in ("linear_x", "angular_z"):
        if field in payload:
            tracked_fields[field] = payload[field]

    if not tracked_fields:
        return None

    return json.dumps(tracked_fields, sort_keys=True, separators=(",", ":"))


def extract_response_command_key(response_data):
    if not isinstance(response_data, dict):
        return None

    velocity = response_data.get("data", {}).get("velocity", {})
    return make_command_key(velocity)


def format_value(value):
    if isinstance(value, (int, float)):
        return f"{value:.2f}"
    return str(value)


def format_command(payload):
    if not isinstance(payload, dict):
        return "payload=N/A"

    return (
        f"linear_x={format_value(payload.get('linear_x', 'N/A'))} "
        f"angular_z={format_value(payload.get('angular_z', 'N/A'))}"
    )


def format_uplink_summary(response_data):
    if not isinstance(response_data, dict):
        return "invalid JSON response"

    code = response_data.get("code", "N/A")
    msg = response_data.get("msg", "N/A")
    velocity = response_data.get("data", {}).get("velocity", {})

    velocity_summary = ""
    if velocity:
        velocity_summary = f" {format_command(velocity)}"

    return f"code={code} msg={msg}{velocity_summary}"


def on_connect(client, userdata, flags, rc):
    if rc == 0:
        print(f"Connected to MQTT broker at {MQTT_BROKER}:{MQTT_PORT}")
        client.subscribe([(DOWNLINK_TOPIC, 1), (UPLINK_TOPIC, 1)])
        print(f"Subscribed to downlink: {DOWNLINK_TOPIC}")
        print(f"Subscribed to uplink:   {UPLINK_TOPIC}")
    else:
        print(f"Failed to connect, return code: {rc}")


def on_message(client, userdata, msg):
    global downlink_echo_count, uplink_response_count
    global last_downlink_echo, last_uplink_response

    payload_text = msg.payload.decode()

    try:
        message_data = json.loads(payload_text)
    except Exception as e:
        print(f"RX {msg.topic}: failed to parse payload: {e}; raw={payload_text}")
        return

    if msg.topic == DOWNLINK_TOPIC:
        with response_lock:
            downlink_echo_count += 1
            last_downlink_echo = message_data
            echo_index = downlink_echo_count
            matched_tx = published_payload_index.get(make_command_key(message_data))

        match_info = f" matches TX #{matched_tx}" if matched_tx is not None else ""
        print(
            f"RX downlink [{echo_index:3d}]: subscribed echo{match_info} "
            f"{format_command(message_data)}"
        )
        return

    if msg.topic == UPLINK_TOPIC:
        with response_lock:
            uplink_response_count += 1
            last_uplink_response = message_data
            response_index = uplink_response_count
            matched_tx = published_payload_index.get(extract_response_command_key(message_data))

        match_info = f" for TX #{matched_tx}" if matched_tx is not None else ""
        print(
            f"RX uplink   [{response_index:3d}]: response{match_info} "
            f"{format_uplink_summary(message_data)}"
        )

        if response_index == 1:
            target = message_data.get("data", {}).get("target", {})
            print(f"  -> Target topic: {target.get('topic', 'N/A')}")
            print(f"  -> Hub ID: {target.get('hub_id', 'N/A')}")
        return

    print(f"RX unknown topic {msg.topic}: {payload_text}")


def main():
    global publish_count

    print("MQTT remote_controller load test")
    print(f"Broker: {MQTT_BROKER}:{MQTT_PORT}")
    print(f"Downlink: {DOWNLINK_TOPIC}")
    print(f"Uplink:   {UPLINK_TOPIC}")
    print(f"Target: {HZ}Hz, duration {DURATION}s")
    print(f"Initial linear_x: {LINEAR_START}, increment/s: {INCREMENT_PER_SECOND}")
    print(f"Fixed angular_z: {ANGULAR_Z}")
    print("=" * 50)

    client = mqtt.Client(client_id="test_mqtt_remote_controller")
    client.on_connect = on_connect
    client.on_message = on_message

    client.connect(MQTT_BROKER, MQTT_PORT, keepalive=60)
    client.loop_start()

    # Wait for connection
    time.sleep(1)

    linear_x = LINEAR_START

    for i in range(1, TOTAL_STEPS + 1):
        payload = {"linear_x": linear_x, "angular_z": ANGULAR_Z}
        json_msg = json.dumps(payload)
        payload_key = make_command_key(payload)

        with response_lock:
            publish_count = i
            published_payload_index[payload_key] = i

        client.publish(DOWNLINK_TOPIC, json_msg, qos=1)

        print(f"TX downlink [{i:3d}/{TOTAL_STEPS}]: {format_command(payload)}")

        if i % HZ == 0:
            with response_lock:
                print(
                    "  Progress: "
                    f"sent={publish_count}, "
                    f"downlink_echoes={downlink_echo_count}, "
                    f"uplink_responses={uplink_response_count}"
                )

        linear_x += INCREMENT_PER_STEP
        time.sleep(1 / HZ)

    # Wait a moment for remaining responses
    time.sleep(0.5)

    client.loop_stop()
    client.disconnect()

    print("=" * 50)
    print(f"Test complete! Sent {TOTAL_STEPS} messages")
    print(f"Downlink echoes received: {downlink_echo_count}")
    print(f"Uplink responses received: {uplink_response_count}")
    print(f"Final linear_x: {linear_x:.2f}")
    if last_downlink_echo is not None:
        print(f"Last downlink echo: {format_command(last_downlink_echo)}")
    if last_uplink_response is not None:
        print(f"Last uplink response: {format_uplink_summary(last_uplink_response)}")
    if downlink_echo_count < TOTAL_STEPS:
        print(f"Warning: {TOTAL_STEPS - downlink_echo_count} downlink echoes missing")
    if uplink_response_count < TOTAL_STEPS:
        print(f"Warning: {TOTAL_STEPS - uplink_response_count} uplink responses missing")


if __name__ == "__main__":
    main()
