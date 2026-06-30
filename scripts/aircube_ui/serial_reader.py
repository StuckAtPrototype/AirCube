"""Serial reading and sensor-line parsing for AirCube."""
import json
import re

from PyQt6.QtCore import QThread, pyqtSignal

import serial

# JSON pattern for parsing sensor data
JSON_PATTERN = re.compile(r"\{.*\}")


def parse_json_line(line):
    """Parse a JSON sensor data line into a flat dict, or None if invalid."""
    match = JSON_PATTERN.search(line)
    if not match:
        return None
    try:
        data = json.loads(match.group(0))
        return {
            "timestamp": data.get("timestamp"),
            "temperature_c": data["ens210"].get("temperature_c"),
            "temperature_f": data["ens210"].get("temperature_f"),
            "humidity": data["ens210"].get("humidity"),
            "ens210_status": data["ens210"].get("status"),
            "ens16x_status": data["ens16x"].get("status"),
            "etvoc": data["ens16x"].get("etvoc"),
            "eco2": data["ens16x"].get("eco2"),
            "aqi": data["ens16x"].get("aqi"),
        }
    except (KeyError, TypeError, json.JSONDecodeError):
        return None


class SerialReaderThread(QThread):
    """Background thread for reading serial data."""
    data_received = pyqtSignal(dict)
    error_occurred = pyqtSignal(str)

    def __init__(self, port, baud=115200):
        super().__init__()
        self.port = port
        self.baud = baud
        self.running = False
        self.serial = None

    def run(self):
        try:
            self.serial = serial.Serial(self.port, self.baud, timeout=0.1)
            self.running = True
            while self.running:
                try:
                    line = self.serial.readline()
                    if line:
                        decoded = line.decode(errors="ignore").strip()
                        parsed = parse_json_line(decoded)
                        if parsed:
                            self.data_received.emit(parsed)
                except (serial.SerialException, OSError) as e:
                    if self.running:
                        self.error_occurred.emit(str(e))
                    break
        except serial.SerialException as e:
            self.error_occurred.emit(str(e))
        finally:
            if self.serial and self.serial.is_open:
                self.serial.close()

    def stop(self):
        self.running = False
        self.wait(2000)
