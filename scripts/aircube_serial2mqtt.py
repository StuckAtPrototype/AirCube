#!/usr/bin/env python3
"""
AirCube -> MQTT Bridge
Reads JSON sensor data from AirCube over USB serial and publishes to MQTT
with Home Assistant auto-discovery support. Accepts brightness commands on
MQTT and forwards them to the device as set_intensity serial commands.

Requires Python 3.2+.

Usage:
    pip install pyserial paho-mqtt python-dotenv
    python aircube_serial2mqtt.py

Config via environment variables or edit the DEFAULTS below.
"""

from __future__ import print_function

import argparse
import collections
import json
import logging
import os
import queue
import sys
import threading
import time

import serial
import paho.mqtt.client as mqtt

try:
    JSON_DECODE_ERROR = json.JSONDecodeError
except AttributeError:
    JSON_DECODE_ERROR = ValueError

SCRIPT_DIR = os.path.dirname(os.path.abspath(__file__))
ENV_FILE = os.path.join(SCRIPT_DIR, ".env")
DOTENV_AVAILABLE = False
ENV_LOADED = False

try:
    from dotenv import load_dotenv

    DOTENV_AVAILABLE = True
    if os.path.isfile(ENV_FILE):
        load_dotenv(ENV_FILE)
        ENV_LOADED = True
except ImportError:
    pass


def log_env_status(logger):
    """Warn at startup if a .env file exists but could not be loaded."""
    if ENV_LOADED:
        logger.info("Loaded config from %s", ENV_FILE)
        return

    if not os.path.isfile(ENV_FILE):
        return

    if DOTENV_AVAILABLE:
        logger.warning(
            "Found %s but dotenv did not load it; using script defaults", ENV_FILE
        )
    else:
        logger.warning(
            "Found %s but python-dotenv is not installed - .env ignored, "
            "using script defaults. Install with: pip install python-dotenv or edit script directly" ,
            ENV_FILE,
        )


def serial_port_open(ser):
    """Return True when a pyserial handle is open (2.x and 3.x compatible)."""
    if ser is None:
        return False
    is_open = getattr(ser, "is_open", None)
    if is_open is not None:
        return is_open
    return ser.isOpen()


# ---------------------------------------------------------------------------
# Configuration - override with environment variables
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
PUBLISH_INTERVAL = int(os.getenv("AIRCUBE_INTERVAL", "0"))

STATE_TOPIC = "aircube/{0}/state".format(DEVICE_ID)
AVAIL_TOPIC = "aircube/{0}/availability".format(DEVICE_ID)
BRIGHTNESS_CMD_TOPIC = "aircube/{0}/brightness/set".format(DEVICE_ID)
BRIGHTNESS_STATE_TOPIC = "aircube/{0}/brightness/state".format(DEVICE_ID)

# ---------------------------------------------------------------------------
# Sensor definitions: (ha_key, unit, device_class, state_class, icon)
# ha_key must match the key we publish into the state JSON payload
# ---------------------------------------------------------------------------
SENSORS = [
    ("temperature_c", "C", "temperature", "measurement", None),
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


class BridgeState(object):
    """Shared state between the serial reader, main loop, and MQTT callbacks."""

    def __init__(self):
        self.ser = None
        self.write_lock = threading.Lock()
        self.intensity_ack = threading.Event()
        self.intensity_value = None
        self.sensor_queue = queue.Queue()
        self.stop = threading.Event()


# ---------------------------------------------------------------------------
# MQTT helpers
# ---------------------------------------------------------------------------
def publish_discovery(client):
    """Publish HA MQTT discovery config for each sensor (retained)."""
    device_info = {
        "identifiers": [DEVICE_ID],
        "name": DEVICE_NAME,
        "model": "AirCube",
        "manufacturer": "StuckAtPrototype",
    }

    for ha_key, unit, device_class, state_class, icon in SENSORS:
        friendly = ha_key.replace("_", " ").replace("c", "").title().strip()
        unique_id = "{0}_{1}".format(DEVICE_ID, ha_key)
        config_topic = "{0}/sensor/{1}/{2}/config".format(
            DISCOVERY_PREFIX, DEVICE_ID, ha_key
        )

        payload = {
            "name": friendly,
            "unique_id": unique_id,
            "state_topic": STATE_TOPIC,
            "value_template": "{{{{ value_json.{0} }}}}".format(ha_key),
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

    brightness_config_topic = "{0}/number/{1}/brightness/config".format(
        DISCOVERY_PREFIX, DEVICE_ID
    )
    brightness_payload = {
        "name": "Brightness",
        "unique_id": "{0}_brightness".format(DEVICE_ID),
        "command_topic": BRIGHTNESS_CMD_TOPIC,
        "state_topic": BRIGHTNESS_STATE_TOPIC,
        "availability_topic": AVAIL_TOPIC,
        "device": device_info,
        "min": 0,
        "max": 1,
        "step": 0.01,
        "mode": "slider",
    }
    client.publish(brightness_config_topic, json.dumps(brightness_payload), retain=True)
    log.info("Discovery published: %s", brightness_config_topic)


def on_connect(client, userdata, flags, rc):
    if rc == 0:
        log.info("MQTT connected to %s:%s", MQTT_HOST, MQTT_PORT)
        publish_discovery(client)
        client.subscribe(BRIGHTNESS_CMD_TOPIC)
        log.info("Subscribed to brightness command topic: %s", BRIGHTNESS_CMD_TOPIC)
        client.publish(AVAIL_TOPIC, "online", retain=True)
    else:
        log.error("MQTT connection failed, rc=%s", rc)


def on_disconnect(client, userdata, rc):
    log.warning("MQTT disconnected (rc=%s), will retry", rc)


def build_mqtt_client(bridge):
    client = mqtt.Client(client_id=DEVICE_ID, clean_session=True, userdata=bridge)
    client.will_set(AVAIL_TOPIC, "offline", retain=True)
    client.on_connect = on_connect
    client.on_disconnect = on_disconnect
    client.on_message = on_message
    if MQTT_USER:
        client.username_pw_set(MQTT_USER, MQTT_PASS)
    return client


# ---------------------------------------------------------------------------
# Serial helpers
# ---------------------------------------------------------------------------
def open_serial(port, baud, retries=10):
    for attempt in range(1, retries + 1):
        try:
            ser = serial.Serial(port, baud, timeout=5)
            log.info("Serial opened: %s @ %d baud", port, baud)
            return ser
        except serial.SerialException as exc:
            log.warning(
                "Serial open failed (attempt %d/%d): %s", attempt, retries, exc
            )
            time.sleep(3)
    log.error("Could not open serial port %s after %d attempts", port, retries)
    sys.exit(1)


def parse_brightness_payload(raw):
    """
    Parse an MQTT brightness command payload.

    Accepts:
      - plain number: "0.3" (0.0-1.0) or "30" (0-100 percent)
      - JSON object: {"value": 0.3} or {"brightness": 30}
    """
    text = raw.strip()
    if not text:
        return None

    try:
        data = json.loads(text)
    except JSON_DECODE_ERROR:
        data = text

    if isinstance(data, dict):
        value = data.get("value", data.get("brightness"))
        if value is None:
            return None
    else:
        value = data

    try:
        intensity = float(value)
    except (TypeError, ValueError):
        return None

    if intensity > 1.0:
        intensity /= 100.0

    return max(0.0, min(1.0, intensity))


def parse_set_intensity_response(raw):
    """Return applied intensity if raw is a set_intensity ack, else None."""
    try:
        parsed = json.loads(raw.strip())
    except JSON_DECODE_ERROR:
        return None

    if parsed.get("status") == "ok" and parsed.get("cmd") == "set_intensity":
        return float(parsed.get("value", 0))
    return None


def dispatch_serial_line(bridge, line):
    """Route one serial line to brightness ack handling or the sensor queue."""
    stripped = line.strip()
    if not stripped:
        return

    applied = parse_set_intensity_response(stripped)
    if applied is not None:
        bridge.intensity_value = applied
        bridge.intensity_ack.set()
        return

    payload = parse_aircube(stripped)
    if payload is not None:
        bridge.sensor_queue.put(payload)
        return

    log.debug("Skipping non-sensor line: %s", stripped)


def serial_reader_loop(bridge):
    """Single reader for all serial input so command acks are never dropped."""
    log.info("Serial reader thread started")
    while not bridge.stop.is_set():
        ser = bridge.ser
        if not serial_port_open(ser):
            time.sleep(0.1)
            continue

        try:
            raw = ser.readline().decode("utf-8", "replace")
            if raw:
                dispatch_serial_line(bridge, raw)
        except serial.SerialException as exc:
            log.error("Serial reader error: %s", exc)
            bridge.sensor_queue.put(None)


def send_set_intensity(bridge, intensity):
    """Send set_intensity to the device and wait for ack from the reader thread."""
    if bridge is None:
        log.error("Internal bridge state missing; cannot set brightness")
        return False

    if not serial_port_open(bridge.ser):
        log.error("Serial port not open; cannot set brightness")
        return False

    # Firmware parser expects compact JSON (no spaces).
    command = json.dumps(
        {"cmd": "set_intensity", "value": intensity},
        separators=(",", ":"),
    ) + "\n"
    bridge.intensity_ack.clear()
    bridge.intensity_value = None

    with bridge.write_lock:
        try:
            bridge.ser.write(command.encode("utf-8"))
            bridge.ser.flush()
            log.info("Sent set_intensity command: %.2f", intensity)
        except serial.SerialException as exc:
            log.error("Serial write failed while setting brightness: %s", exc)
            return False

    if bridge.intensity_ack.wait(5.0):
        log.info("Brightness set to %.2f via serial", bridge.intensity_value)
        return True

    log.warning("Timed out waiting for set_intensity response (command was sent)")
    return False


def on_message(client, userdata, msg):
    if msg.topic != BRIGHTNESS_CMD_TOPIC:
        return

    intensity = parse_brightness_payload(msg.payload.decode("utf-8", "replace"))
    if intensity is None:
        log.warning("Invalid brightness payload on %s: %r", msg.topic, msg.payload)
        return

    bridge = userdata
    if not isinstance(bridge, BridgeState):
        log.error(
            "MQTT userdata is not bridge state (%r); cannot set brightness", userdata
        )
        return

    if send_set_intensity(bridge, intensity):
        applied = (
            bridge.intensity_value
            if bridge.intensity_value is not None
            else intensity
        )
        client.publish(BRIGHTNESS_STATE_TOPIC, "{0:.2f}".format(applied), retain=True)


def parse_aircube(raw):
    """
    Parse one line of AirCube JSON output into a flat dict for MQTT.
    Returns None if the line isn't valid AirCube data.
    """
    try:
        data = json.loads(raw.strip())
    except JSON_DECODE_ERROR:
        return None

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
def main():
    parser = argparse.ArgumentParser(description="AirCube to MQTT Bridge")
    parser.add_argument(
        "--no-mqtt", action="store_true", help="Don't connect to MQTT, just print data"
    )
    parser.add_argument(
        "--interval",
        type=int,
        default=PUBLISH_INTERVAL,
        help=(
            "Publish interval in seconds. Set to 0 to publish every packet "
            "immediately. (default: {0})".format(PUBLISH_INTERVAL)
        ),
    )
    args = parser.parse_args()

    log.info("Starting AirCube MQTT bridge (device_id=%s)", DEVICE_ID)
    log_env_status(log)
    log.info("Serial: %s", SERIAL_PORT)
    log.info("Interval: %s seconds", args.interval)

    bridge = BridgeState()
    client = None
    if not args.no_mqtt:
        log.info("MQTT: %s:%s  State topic: %s", MQTT_HOST, MQTT_PORT, STATE_TOPIC)
        log.info("Brightness command topic: %s", BRIGHTNESS_CMD_TOPIC)
        client = build_mqtt_client(bridge)
        try:
            client.connect(MQTT_HOST, MQTT_PORT, keepalive=60)
        except Exception as exc:
            log.error("Could not connect to MQTT broker: %s", exc)
            sys.exit(1)
        client.loop_start()
    else:
        log.info("MQTT disabled: running in print-only mode")

    ser = open_serial(SERIAL_PORT, SERIAL_BAUD)
    bridge.ser = ser

    reader = threading.Thread(target=serial_reader_loop, args=(bridge,))
    reader.daemon = True
    reader.start()

    consecutive_errors = 0
    buffer = collections.defaultdict(list)
    last_publish_time = time.time()

    while True:
        try:
            try:
                payload = bridge.sensor_queue.get(True, 1)
            except queue.Empty:
                continue

            if payload is None:
                raise serial.SerialException("serial reader reported disconnect")

            if args.interval <= 0:
                if client:
                    client.publish(STATE_TOPIC, json.dumps(payload))
                    log.info("Published data: %s", payload)
                else:
                    print(
                        "WOULD PUBLISH data to {0}: {1}".format(
                            STATE_TOPIC, json.dumps(payload)
                        )
                    )
                continue

            for key, val in payload.items():
                if val is not None:
                    buffer[key].append(val)

            current_time = time.time()
            if current_time - last_publish_time >= args.interval:
                if buffer:
                    avg_payload = {}
                    for key, values in buffer.items():
                        avg_payload[key] = round(sum(values) / float(len(values)), 2)

                    if client:
                        client.publish(STATE_TOPIC, json.dumps(avg_payload))
                        log.info("Published averaged data: %s", avg_payload)
                    else:
                        print(
                            "WOULD PUBLISH averaged data to {0}: {1}".format(
                                STATE_TOPIC, json.dumps(avg_payload)
                            )
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
            bridge.ser = None
            time.sleep(5)
            ser = open_serial(SERIAL_PORT, SERIAL_BAUD)
            bridge.ser = ser
            if client:
                client.publish(AVAIL_TOPIC, "online", retain=True)

        except KeyboardInterrupt:
            log.info("Shutting down")
            bridge.stop.set()
            if client:
                client.publish(AVAIL_TOPIC, "offline", retain=True)
                client.loop_stop()
            ser.close()
            sys.exit(0)


if __name__ == "__main__":
    main()
