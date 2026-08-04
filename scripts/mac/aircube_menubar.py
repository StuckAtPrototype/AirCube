"""
AirCube MenuBar Monitor
A lightweight menu bar app that displays AQI in the Mac OS menu bar.
"""

__version__ = "1.0.0"
__app_name__ = "AirCube MenuBar"

import json
import re
import threading
import time

import rumps
import serial
from serial.tools import list_ports

JSON_PATTERN = re.compile(r"\{.*\}")


def parse_json_line(line):
	match = JSON_PATTERN.search(line)
	if not match:
		return None

	try:
		data = json.loads(match.group(0))

		return {
			"temperature_c": data["ens210"].get("temperature_c"),
			"humidity": data["ens210"].get("humidity"),
			"aqi": data["ens16x"].get("aqi"),
			"aqi_uba": data["ens16x"].get("aqi_uba"),
			"eco2": data["ens16x"].get("eco2"),
			"etvoc": data["ens16x"].get("etvoc"),
		}
	except Exception:
		return None


class AirCubeMenuBar(rumps.App):
	def __init__(self):
		super().__init__("AirCube", title="AQI --", quit_button=None)

		self.port = None
		self.serial = None
		self.running = False
		self.latest = {}

		self.menu = [
			rumps.MenuItem("Status: Disconnected"),
			rumps.MenuItem("Port: --"),
			None,
			rumps.MenuItem("Connect", callback=self.connect),
			rumps.MenuItem("Disconnect", callback=self.disconnect),
			rumps.MenuItem("Refresh Ports", callback=self.refresh_ports),
			None,
			rumps.MenuItem("Quit", callback=self.quit_app),
		]

		self.refresh_ports(None)

	def refresh_ports(self, _):
		ports = list(list_ports.comports())

		port_menu = rumps.MenuItem("Select Port")
		for port in ports:
			port_menu.add(rumps.MenuItem(port.device, callback=self.select_port))

		if not ports:
			port_menu.add(rumps.MenuItem("No ports found"))

		self.menu["Select Port"] = port_menu

		if not self.port and ports:
			self.port = ports[0].device
			self.menu["Port: --"].title = f"Port: {self.port}"

	def select_port(self, sender):
		self.port = sender.title
		self.menu["Port: --"].title = f"Port: {self.port}"

	def connect(self, _):
		if not self.port:
			rumps.alert("No AirCube serial port selected.")
			return

		if self.running:
			return

		self.running = True
		threading.Thread(target=self.read_serial, daemon=True).start()

	def disconnect(self, _):
		self.running = False

		if self.serial and self.serial.is_open:
			self.serial.close()

		self.title = "AQI --"
		self.menu["Status: Disconnected"].title = "Status: Disconnected"

	def read_serial(self):
		try:
			self.serial = serial.Serial(self.port, 115200, timeout=0.5)
			self.menu["Status: Disconnected"].title = "Status: Connected"

			while self.running:
				line = self.serial.readline()

				if not line:
					continue

				decoded = line.decode(errors="ignore").strip()
				data = parse_json_line(decoded)

				if data:
					self.latest = data
					self.update_menu(data)

		except Exception as e:
			self.menu["Status: Disconnected"].title = f"Error: {e}"
			self.title = "AQI !"
			self.running = False

	def update_menu(self, data):
		aqi = data.get("aqi")
		temp_c = data.get("temperature_c")
		humidity = data.get("humidity")
		eco2 = data.get("eco2")
		etvoc = data.get("etvoc")

		if aqi is not None:
			self.title = f"AQI {int(aqi)}"

		temp_f = None
		if temp_c is not None:
			temp_f = temp_c * 9 / 5 + 32

		self.menu["Status: Disconnected"].title = "Status: Connected"
		self.menu["AQI"] = f"AQI: {int(aqi) if aqi is not None else '--'}"
		self.menu["Temperature"] = f"Temperature: {temp_f:.1f}°F" if temp_f is not None else "Temperature: --"
		self.menu["Humidity"] = f"Humidity: {humidity:.1f}%" if humidity is not None else "Humidity: --"
		self.menu["eCO2"] = f"eCO2: {int(eco2)} ppm" if eco2 is not None else "eCO2: --"
		self.menu["eTVOC"] = f"eTVOC: {int(etvoc)} ppb" if etvoc is not None else "eTVOC: --"

	def quit_app(self, _):
		self.disconnect(None)
		rumps.quit_application()


if __name__ == "__main__":
	AirCubeMenuBar().run()