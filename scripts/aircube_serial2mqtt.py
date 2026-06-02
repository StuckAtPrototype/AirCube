#!/usr/bin/env python3
"""
AirCube → MQTT Bridge
Reads JSON sensor data from AirCube over USB serial and publishes to MQTT
with Home Assistant auto-discovery support.

Usage:
    pip install pyserial paho-mqtt
    python aircube_mqtt_bridge.py

Config via environment variables or edit the DEFAULTS below.
"""

import argparse
import collections
import json
import logging
import os
import sys
import time
import serial
import paho.mqtt.client as mqtt

# ---------------------------------------------------------------------------
# Configuration — override with environment variables
# ---------------------------------------------------------------------------
SERIAL_PORT = os.getenv("AIRCUBE_PORT", "/dev/cu.usbmodem101")
SERIAL_BAUD = int(os.getenv("AIRCUBE_BAUD", "115200"))
MQTT_HOST = os.getenv("MQTT_HOST", "localhost")
MQTT_PORT = int(os.getenv("MQTT_PORT", "1883"))
MQTT_USER = os.getenv("MQTT_USER", "")
MQTT_PASS = os.getenv("MQTT_PASS", "")
DEVICE_NAME = os.getenv("AIRCUBE_NAME", "AirCube")
DEVICE_ID = os.getenv("AIRCUBE_ID", "aircube_1")  # unique per device
DISCOVERY_PREFIX = os.getenv("HA_DISCOVERY_PREFIX", "homeassistant")

STATE_TOPIC = f"aircube/{DEVICE_ID}/state"
AVAIL_TOPIC = f"aircube/{DEVICE_ID}/availability"

# ---------------------------------------------------------------------------
# Sensor definitions: (ha_key, unit, device_class, state_class, icon)
# ha_key must match the key we publish into the state JSON payload
# ---------------------------------------------------------------------------
SENSORS = [
    ("temperature_c", "°C", "temperature", "measurement", None),
    ("humidity", "%", "humidity", "measurement", None),
    ("eco2", "ppm", "carbon_dioxide", "measurement", None),
    ("etvoc", "ppb", None, "measurement", "mdi:chemical-weapon"),
    ("aqi", None, "aqi", "measurement", None),
]

logging.basicConfig(
    level=logging.INFO,
    format="%(asctime)s [%(levelname)s] %(message)s",
    datefmt="%Y-%m-%d %H:%M:%S",
)
log = logging.getLogger(__name__)


# ---------------------------------------------------------------------------
# MQTT helpers
# ---------------------------------------------------------------------------
def publish_discovery(client: mqtt.Client) -> None:
    """Publish HA MQTT discovery config for each sensor (retained)."""
    device_info = {
        "identifiers": [DEVICE_ID],
        "name": DEVICE_NAME,
        "model": "AirCube",
        "manufacturer": "StuckAtPrototype",
    }

    for ha_key, unit, device_class, state_class, icon in SENSORS:
        friendly = ha_key.replace("_", " ").replace("c", "").title().strip()
        unique_id = f"{DEVICE_ID}_{ha_key}"
        config_topic = f"{DISCOVERY_PREFIX}/sensor/{DEVICE_ID}/{ha_key}/config"

        payload: dict = {
            "name": friendly,
            "unique_id": unique_id,
            "state_topic": STATE_TOPIC,
            "value_template": f"{{{{ value_json.{ha_key} }}}}",
            "availability_topic": AVAIL_TOPIC,
            "device": device_info,
            "state_class": state_class,
        }
        if unit:
            payload["unit_of_measurement"] = unit
        if device_class:
            payload["device_class"] = device_class
        if icon:
            payload["icon"] = icon

        client.publish(config_topic, json.dumps(payload), retain=True)
        log.info("Discovery published: %s", config_topic)


def on_connect(client: mqtt.Client, userdata, flags, rc) -> None:
    if rc == 0:
        log.info("MQTT connected to %s:%s", MQTT_HOST, MQTT_PORT)
        publish_discovery(client)
        client.publish(AVAIL_TOPIC, "online", retain=True)
    else:
        log.error("MQTT connection failed, rc=%s", rc)


def on_disconnect(client: mqtt.Client, userdata, rc) -> None:
    log.warning("MQTT disconnected (rc=%s), will retry", rc)


def build_mqtt_client() -> mqtt.Client:
    client = mqtt.Client(client_id=DEVICE_ID, clean_session=True)
    client.will_set(AVAIL_TOPIC, "offline", retain=True)
    client.on_connect = on_connect
    client.on_disconnect = on_disconnect
    if MQTT_USER:
        client.username_pw_set(MQTT_USER, MQTT_PASS)
    return client


# ---------------------------------------------------------------------------
# Serial helpers
# ---------------------------------------------------------------------------
def open_serial(port: str, baud: int, retries: int = 10) -> serial.Serial:
    for attempt in range(1, retries + 1):
        try:
            ser = serial.Serial(port, baud, timeout=5)
            log.info("Serial opened: %s @ %d baud", port, baud)
            return ser
        except serial.SerialException as exc:
            log.warning("Serial open failed (attempt %d/%d): %s", attempt, retries, exc)
            time.sleep(3)
    log.error("Could not open serial port %s after %d attempts", port, retries)
    sys.exit(1)


def parse_aircube(raw: str) -> dict | None:
    """
    Parse one line of AirCube JSON output into a flat dict for MQTT.
    Returns None if the line isn't valid AirCube data.

    Expected input:
      {"ens210": {"temperature_c": 23.45, "humidity": 52.30},
       "ens16x": {"etvoc": 42, "eco2": 415, "aqi": 3},
       "timestamp": 12345}
    """
    try:
        data = json.loads(raw.strip())
    except json.JSONDecodeError:
        return None

    # Must have both sensor blocks
    if "ens210" not in data or "ens16x" not in data:
        return None

    return {
        "temperature_c": data["ens210"].get("temperature_c"),
        "humidity": data["ens210"].get("humidity"),
        "eco2": data["ens16x"].get("eco2"),
        "etvoc": data["ens16x"].get("etvoc"),
        "aqi": data["ens16x"].get("aqi"),
    }


# ---------------------------------------------------------------------------
# Main loop
# ---------------------------------------------------------------------------
def main() -> None:
    parser = argparse.ArgumentParser(description="AirCube to MQTT Bridge")
    parser.add_argument(
        "--no-mqtt", action="store_true", help="Don't connect to MQTT, just print data"
    )
    args = parser.parse_args()

    log.info("Starting AirCube MQTT bridge (device_id=%s)", DEVICE_ID)
    log.info("Serial: %s", SERIAL_PORT)

    client = None
    if not args.no_mqtt:
        log.info("MQTT: %s:%s  State topic: %s", MQTT_HOST, MQTT_PORT, STATE_TOPIC)
        client = build_mqtt_client()
        # Connect MQTT (non-blocking loop in background thread)
        try:
            client.connect(MQTT_HOST, MQTT_PORT, keepalive=60)
        except Exception as exc:
            log.error("Could not connect to MQTT broker: %s", exc)
            sys.exit(1)
        client.loop_start()
    else:
        log.info("MQTT disabled: running in print-only mode")

    ser = open_serial(SERIAL_PORT, SERIAL_BAUD)

    consecutive_errors = 0
    buffer = collections.defaultdict(list)
    last_publish_time = time.time()

    while True:
        try:
            raw = ser.readline().decode("utf-8", errors="replace")
            if not raw.strip():
                continue

            payload = parse_aircube(raw)
            if payload is None:
                log.debug("Skipping non-sensor line: %s", raw.strip())
                continue

            # Add values to buffer
            for key, val in payload.items():
                if val is not None:
                    buffer[key].append(val)

            current_time = time.time()
            if current_time - last_publish_time >= 10:
                if buffer:
                    # Calculate averages
                    avg_payload = {}
                    for key, values in buffer.items():
                        avg_payload[key] = round(sum(values) / len(values), 2)

                    if client:
                        client.publish(STATE_TOPIC, json.dumps(avg_payload))
                        log.info("Published averaged data: %s", avg_payload)
                    else:
                        print(
                            f"WOULD PUBLISH averaged data to {STATE_TOPIC}: {json.dumps(avg_payload)}"
                        )

                    buffer.clear()

                last_publish_time = current_time

            consecutive_errors = 0

        except serial.SerialException as exc:
            consecutive_errors += 1
            log.error("Serial error: %s (attempt %d)", exc, consecutive_errors)
            if client:
                client.publish(AVAIL_TOPIC, "offline", retain=True)
            ser.close()
            time.sleep(5)
            ser = open_serial(SERIAL_PORT, SERIAL_BAUD)
            if client:
                client.publish(AVAIL_TOPIC, "online", retain=True)

        except KeyboardInterrupt:
            log.info("Shutting down")
            if client:
                client.publish(AVAIL_TOPIC, "offline", retain=True)
                client.loop_stop()
            ser.close()
            sys.exit(0)


if __name__ == "__main__":
    main()
