#!/usr/bin/env python3
"""
Falcon OV519 mode — PC-side protocol test.

Replays what the Xbox camera driver does over USB (from the Darkone83 research):
the OV519 bridge/SCCB register handshake to detect the OV7648 sensor, then starts
streaming. Confirms our device answers the exact sequence the Xbox will use.

Requires the "Xbox Video Camera" (045E:028C) bound to WinUSB (run Zadig once) and
pyusb + libusb-package. Watch the ESP32 UART log alongside: selecting alt 3 should
print "stream start: alt 3".

    python tools/ov519_pctest.py
"""
import sys
import time

import usb.core
import usb.util
import libusb_package

VID, PID = 0x045E, 0x028C
OVW = 0x40  # vendor, host->device: write bridge register
OVR = 0xC0  # vendor, device->host: read  bridge register
BREQ = 0x01

# Expected OV7648 identity (what our emulation returns).
EXPECT = {0x1C: 0x7F, 0x1D: 0xA2, 0x0A: 0x76, 0x0B: 0x48}


def ov_w(dev, reg, val):
    dev.ctrl_transfer(OVW, BREQ, 0x0000, reg, bytes([val]))


def ov_r(dev, reg):
    return int(dev.ctrl_transfer(OVR, BREQ, 0x0000, reg, 1)[0])


def sccb_read(dev, reg):
    """Read an OV7648 sensor register through the OV519 SCCB engine."""
    ov_w(dev, 0x43, reg)   # read sub-address
    ov_w(dev, 0x47, 0x03)  # initiate read
    # poll status (bit0 == 0 -> done); our emulation always reports done
    for _ in range(10):
        if (ov_r(dev, 0x47) & 0x01) == 0:
            break
    ov_w(dev, 0x47, 0x05)  # second read phase
    return ov_r(dev, 0x45)  # latched result


def main():
    dev = usb.core.find(idVendor=VID, idProduct=PID,
                        backend=libusb_package.get_libusb1_backend())
    if dev is None:
        print(f"FAIL: device {VID:04X}:{PID:04X} not found. "
              f"Bind it to WinUSB with Zadig first.")
        return 2
    print(f"found {VID:04X}:{PID:04X} — {usb.util.get_string(dev, dev.iProduct)}")

    try:
        dev.set_configuration()
    except usb.core.USBError:
        pass  # already configured

    # --- OV519 bridge bring-up (subset that matters for detection) ---
    ov_w(dev, 0x72, 0xEE)  # GPIO: clear bit4 so the sensor is detectable
    ov_w(dev, 0x41, 0x42)  # SCCB write slave id
    ov_w(dev, 0x44, 0x43)  # SCCB read  slave id
    rb = ov_r(dev, 0x72)
    print(f"bridge reg 0x72 readback = 0x{rb:02X} (wrote 0xEE)")

    # --- Sensor identity handshake ---
    ok = True
    for reg, exp in EXPECT.items():
        got = sccb_read(dev, reg)
        tag = "ok" if got == exp else "MISMATCH"
        if got != exp:
            ok = False
        print(f"sensor reg 0x{reg:02X} = 0x{got:02X} (expect 0x{exp:02X}) [{tag}]")
    print("OV7648 detect:", "PASS — control emulation matches the Xbox path"
          if ok else "FAIL")

    # --- Start streaming (alt 3 = 320x240, iso maxpkt 768) ---
    print("SET_INTERFACE(0, alt 3) — watch the UART for 'stream start: alt 3'")
    try:
        dev.set_interface_altsetting(interface=0, alternate_setting=3)
        print("  SET_INTERFACE ok")
    except usb.core.USBError as e:
        print(f"  SET_INTERFACE error: {e}")

    # --- Best-effort iso read (pyusb iso on Windows is unreliable; the real
    #     iso proof is the Xbox). Report whatever comes back. ---
    print("attempting iso IN read on EP 0x81 (best-effort)...")
    got_data = 0
    t0 = time.time()
    while time.time() - t0 < 2.0:
        try:
            data = dev.read(0x81, 768 * 8, timeout=200)
            if data:
                got_data += len(data)
                head = " ".join(f"{b:02x}" for b in data[:8])
                print(f"  iso read {len(data)} bytes, head: {head}")
                break
        except usb.core.USBError:
            continue
    if not got_data:
        print("  no iso data via pyusb (expected on Windows) — "
              "confirm streaming via the UART log instead")

    dev.set_interface_altsetting(interface=0, alternate_setting=0)  # stop
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
