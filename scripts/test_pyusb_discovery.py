#!/usr/bin/env python3
"""Minimal test: can the AirCube be found and read via pyusb (libusb)?

This is the direct counterpart to test_serial_discovery.py. Instead of relying
on a /dev/ttyACM* serial node (which Crostini often never creates for the
ESP32-H2's native USB Serial/JTAG), it talks to the raw USB device through
libusb. If this script finds the device and reads JSON lines while the serial
script finds nothing, then switching aircube_app.py to pyusb is the fix.

Run:  python3 test_pyusb_discovery.py
Deps: pyusb   (pip install pyusb)   + a libusb backend
        - Debian/Crostini: sudo apt install libusb-1.0-0
        - You may need a udev rule so the device is readable without root:
            SUBSYSTEM=="usb", ATTR{idVendor}=="303a", MODE="0666"
          (/etc/udev/rules.d/99-aircube.rules, then `sudo udevadm trigger`)
          or just run this script with sudo for a quick test.
"""

import os
import sys
import time

try:
    import usb.core
    import usb.util
except ImportError:
    sys.exit("pyusb is not installed. Run: pip install pyusb")

# ESP32-H2 USB Serial/JTAG ("USB JTAG/serial debug unit").
ESPRESSIF_VID = 0x303A
JTAG_SERIAL_PID = 0x1001
READ_SECONDS = 12


def main():
    print("=== pyusb discovery (raw libusb, bypasses /dev/ttyACM) ===\n")

    # 1. Enumerate everything so we can see what libusb sees at all.
    all_devs = list(usb.core.find(find_all=True))
    if not all_devs:
        print("libusb found NO usb devices at all.")
        print("On Crostini, make sure the AirCube is shared into Linux:")
        print("  ChromeOS Settings > 'USB JTAG/serial debug unit' > toggle into Linux.")
        return

    print(f"libusb sees {len(all_devs)} USB device(s):")
    for d in all_devs:
        print(f"  VID:PID = 0x{d.idVendor:04X}:0x{d.idProduct:04X}")
    print()

    # 2. Find the AirCube specifically.
    dev = usb.core.find(idVendor=ESPRESSIF_VID, idProduct=JTAG_SERIAL_PID)
    if dev is None:
        # Fall back to any Espressif device in case the PID differs.
        dev = usb.core.find(idVendor=ESPRESSIF_VID)
    if dev is None:
        print("--> No Espressif (VID 0x303A) device found via libusb either.")
        print("    The device is likely not shared into the Linux container yet.")
        return

    print(f"--> Found AirCube: 0x{dev.idVendor:04X}:0x{dev.idProduct:04X}")

    try:
        cfg = dev.get_active_configuration()
    except usb.core.USBError:
        dev.set_configuration()
        cfg = dev.get_active_configuration()

    # 3. Dump the real descriptor. The ESP32-H2 USB Serial/JTAG enumerates as a
    #    VENDOR-SPECIFIC device (every interface class 0xFF), not standard CDC --
    #    which is exactly why Crostini's cdc-acm driver never created /dev/ttyACM
    #    and pyserial saw nothing. Layout:
    #      interface 0: interrupt IN  -> notifications / control
    #      interface 1: bulk OUT+IN   -> the serial console stream (our JSON)  <-- want
    #      interface 2: bulk OUT+IN   -> JTAG                                  <-- skip
    #    So we pick the FIRST interface that has both a bulk IN and bulk OUT.
    print("\n    --- configuration descriptor ---")
    data_intf = None       # interface object that owns the serial bulk IN
    ep_in = None
    comm_intf = 0          # interrupt/control interface = DTR/RTS target
    for intf in cfg:
        print(f"    interface {intf.bInterfaceNumber} alt {intf.bAlternateSetting} "
              f"class 0x{intf.bInterfaceClass:02X} subclass 0x{intf.bInterfaceSubClass:02X}")
        bulk_in = bulk_out = None
        for ep in intf:
            direction = "IN" if usb.util.endpoint_direction(ep.bEndpointAddress) == usb.util.ENDPOINT_IN else "OUT"
            ep_type = {0: "control", 1: "iso", 2: "bulk", 3: "interrupt"}[usb.util.endpoint_type(ep.bmAttributes)]
            print(f"        ep 0x{ep.bEndpointAddress:02X} {direction} {ep_type} maxpkt {ep.wMaxPacketSize}")
            if usb.util.endpoint_type(ep.bmAttributes) == usb.util.ENDPOINT_TYPE_BULK:
                if usb.util.endpoint_direction(ep.bEndpointAddress) == usb.util.ENDPOINT_IN:
                    bulk_in = ep
                else:
                    bulk_out = ep
        # First interface carrying both bulk IN and bulk OUT = the serial port.
        if ep_in is None and bulk_in is not None and bulk_out is not None:
            ep_in = bulk_in
            data_intf = intf
    print("    --------------------------------\n")

    if ep_in is None:
        print("--> No bulk IN endpoint found on any interface. Cannot read.")
        return
    print(f"    data: bulk IN 0x{ep_in.bEndpointAddress:02X} on interface {data_intf.bInterfaceNumber}")
    print(f"    comm: control interface {comm_intf}")

    # 4. Detach any kernel driver and claim the data interface so our IN tokens
    #    actually pull bytes (an unclaimed interface can let the kernel race us).
    for n in {data_intf.bInterfaceNumber, comm_intf}:
        if n is None:
            continue
        try:
            if dev.is_kernel_driver_active(n):
                dev.detach_kernel_driver(n)
                print(f"    detached kernel driver from interface {n}")
        except (NotImplementedError, usb.core.USBError):
            pass
    for n in (data_intf.bInterfaceNumber, comm_intf):
        try:
            usb.util.claim_interface(dev, n)
            print(f"    claimed interface {n}")
        except usb.core.USBError as e:
            print(f"    warning: could not claim interface {n} ({e})")

    # 5. The ESP32-H2 docs say TX flushes after a newline and the device waits
    #    ~50ms for the host to request data -- i.e. actively reading the IN
    #    endpoint *should* be enough, no DTR needed. So by DEFAULT we do a clean
    #    read-only test and dump RAW HEX of whatever arrives (not just parsed
    #    JSON), to distinguish "no bytes at all" from "bytes that didn't parse".
    #
    #    The DTR/CDC handshake (which keeps timing out through Crostini) is only
    #    attempted if you set TRY_DTR=1, so it can't wedge the endpoint by default.
    if os.environ.get("TRY_DTR") == "1":
        try:
            coding = dev.ctrl_transfer(0xA1, 0x21, 0, comm_intf, 7, timeout=2000)
            print(f"    GET_LINE_CODING -> {bytes(coding).hex()}")
        except usb.core.USBError as e:
            print(f"    GET_LINE_CODING failed ({e})")
        for wValue, label in [(0x0003, "DTR|RTS"), (0x0001, "DTR")]:
            try:
                dev.ctrl_transfer(0x21, 0x22, wValue, comm_intf, None, timeout=3000)
                print(f"    SET_CONTROL_LINE_STATE OK [{label}]")
                break
            except usb.core.USBError as e:
                print(f"    SET_CONTROL_LINE_STATE failed [{label}]: {e}")

    # Clear any stale halt/toggle on the IN endpoints before reading.
    in_eps = [ep_in.bEndpointAddress]
    if ep_in.bEndpointAddress != 0x83:
        in_eps.append(0x83)  # also probe the JTAG IN endpoint, just in case
    for addr in in_eps:
        try:
            dev.clear_halt(addr)
        except usb.core.USBError:
            pass

    print(f"\n    Reading for {READ_SECONDS}s (move/breathe near the sensor)...")
    print(f"    polling IN endpoints: {', '.join(f'0x{a:02X}' for a in in_eps)}\n")
    buf = bytearray()
    total_bytes = 0
    deadline = time.monotonic() + READ_SECONDS
    while time.monotonic() < deadline:
        for addr in in_eps:
            try:
                chunk = dev.read(addr, 64, timeout=200)
            except usb.core.USBError as e:
                if e.errno == 110:  # timeout/NAK, keep polling
                    continue
                print(f"    read error on 0x{addr:02X}: {e}")
                continue
            if chunk:
                raw = chunk.tobytes()
                total_bytes += len(raw)
                # Show raw hex so we see ANY traffic, even non-JSON.
                print(f"    RX 0x{addr:02X} [{len(raw)}B]: {raw.hex()}")
                buf.extend(raw)
                while b"\n" in buf:
                    line, _, buf = buf.partition(b"\n")
                    text = line.decode(errors="ignore").strip()
                    if text:
                        print(f"        parsed: {text}")

    print()
    if total_bytes:
        print(f"--> SUCCESS: read {total_bytes} bytes directly via pyusb.")
        print("    Confirms pyusb is a viable replacement for the serial path.")
    else:
        print("--> No bytes on any IN endpoint. Re-run with TRY_DTR=1 to test whether")
        print("    asserting DTR is required:  sudo TRY_DTR=1 .../python .../test_pyusb_discovery.py")


if __name__ == "__main__":
    main()
