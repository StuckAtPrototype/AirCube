#!/usr/bin/env python3
"""Minimal test: can the AirCube be found via pyserial?

This mirrors exactly what aircube_app.py does to discover the device:
it calls serial.tools.list_ports.comports() and lists what it finds.

On a Chromebook (Crostini), the ESP32-H2's native USB Serial/JTAG device
often does NOT show up here, because no cdc-acm /dev/ttyACM* node is created
in the Linux container. If this script prints "No serial ports found" (or
lists ports but none from Espressif), that confirms the pyserial path is the
problem.

Run:  python3 test_serial_discovery.py
Deps: pyserial   (already a dependency of aircube_app.py)
"""

from serial.tools import list_ports

# ESP32-H2 USB Serial/JTAG = Espressif vendor ID. PID for the
# "USB JTAG/serial debug unit" is 0x1001.
ESPRESSIF_VID = 0x303A


def main():
    print("=== pyserial discovery (same call aircube_app.py uses) ===\n")
    ports = list(list_ports.comports())

    if not ports:
        print("No serial ports found.")
        print("\n--> pyserial cannot see ANY device. This is the failure")
        print("    the AirCube app hits on the Chromebook. Try test_pyusb_discovery.py.")
        return

    found_esp = False
    for p in ports:
        vid = f"0x{p.vid:04X}" if p.vid is not None else "----"
        pid = f"0x{p.pid:04X}" if p.pid is not None else "----"
        print(f"  {p.device}")
        print(f"      description : {p.description}")
        print(f"      hwid        : {p.hwid}")
        print(f"      VID:PID     : {vid}:{pid}")
        if p.vid == ESPRESSIF_VID:
            found_esp = True
            print("      ^^^ This is an Espressif device (likely the AirCube).")
        print()

    if found_esp:
        print("--> An Espressif serial port WAS found. pyserial should work here.")
    else:
        print("--> Ports exist, but NONE are Espressif (VID 0x303A).")
        print("    The AirCube is not exposed as a serial port. Try test_pyusb_discovery.py.")


if __name__ == "__main__":
    main()
