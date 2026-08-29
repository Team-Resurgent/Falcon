# Falcon — Xbox Video Camera emulator (ESP32-S3)

Falcon makes an ESP32-S3 present itself as the **original Xbox Video Camera**
(OmniVision **OV530 / OV519**, the same bridge as the Sony EyeToy) so an
**unmodified** host — the Xbox Video Chat app, or Linux `gspca_ov519` on a PC —
sees a real camera. No app patching.

**Milestone 1 (this repo):** stream a scrolling-checkerboard **MJPEG** at 320×240
so you can *see* video end to end. Audio (the separate Xbox Communicator, a.k.a.
"Hawk") and the Xbox-side capture sample are future work.

## How it works

The Xbox Video Camera is a **vendor-specific** USB device (interface class `0xFF`,
one isochronous IN endpoint `0x81` with five alt-settings), **not** UVC. Its OV519
bridge has an on-chip JPEG engine, so the iso stream is Motion-JPEG framed with
OV519 markers (`FF FF FF 50` start-of-frame, `FF FF FF 51` end-of-frame).

Falcon reproduces this:
- **`usb_descriptors.c`** — byte-faithful device/config/string descriptors
  (VID/PID `045E:028C`, "Microsoft" / "Xbox Video Camera", the 5 iso alt-settings).
- **`ov519_control.c`** — answers the OV519 vendor register protocol (bRequest
  `0x01`) and fakes the OV7648 SCCB sensor so the host's detect/init passes
  (sensor IDs `0x1C/0x1D = 0x7F/0xA2`, `0x0A/0x0B = 0x76/0x48`).
- **`ov519_stream.c`** — packetizes the MJPEG frames with OV519 SOF/EOF framing.
- **`falcon_class.c`** — a custom TinyUSB class driver: owns interface 0, handles
  `SET_INTERFACE` (alt→bandwidth), drives the iso IN endpoint.
- **`tools/gen_checkerboard_jpegs.py`** — pre-encodes the checkerboard frames
  into `main/checkerboard_jpegs.h`.

Spec + provenance: the reverse-engineering behind this is the Darkone83 /
Team Resurgent "Xbox-live-camera-research-project" (EEPROM descriptor, OV519 init,
MJPEG framing).

## Build & flash

ESP-IDF ≥ 5.5, ESP32-S3. The **native USB port is the camera**, so flash/log over
the **UART bridge (COM3)**:

```bash
idf.py set-target esp32s3
idf.py -p COM3 build flash monitor
```

Pick the USB identity in `menuconfig` → *Falcon camera emulator*:
- **Xbox Video Camera (045E:028C)** — for the real Xbox.
- **Sony EyeToy (054C:0155)** — for previewing on a Linux PC (gspca matches this id).

To regenerate the frames (e.g. different pattern/size):
```bash
python tools/gen_checkerboard_jpegs.py --out main/checkerboard_jpegs.h
```

The project builds with **no extra setup** — the two things below are handled for
you or are off by default:

- **dwc2 EP0-SETUP-DMA crash fix (automatic).** `esp_tinyusb` is a managed
  component (re-downloaded pristine), so a required one-line ESP32-S3 fix in its
  `dcd_dwc2.c` is re-applied by `patches/apply_tinyusb_patch.py`, which the build
  runs for you every configure (idempotent). Without it, DMA-mode streaming
  crashes ~2–3 s in on the Xbox. Details in [`patches/README.md`](patches/README.md).
- **WiFi debug logging (off by default).** No `wifi_creds.h` is needed to build.
  To mirror logs over UDP while the board is on a distant Xbox, enable
  *Falcon camera emulator → WiFi UDP live-log broadcast* in `menuconfig`, then
  `cp main/wifi_creds.h.example main/wifi_creds.h` and fill in your SSID/password
  (`wifi_creds.h` is gitignored). Keep it **off** for real Xbox tests — joining
  WiFi breaks the Xbox's timing-strict USB enumeration.

## Verify on a PC (no Xbox) — Linux

Build with the **EyeToy** identity (`menuconfig` → *Falcon camera emulator* →
*USB identity* → Sony EyeToy) and plug the **native USB** port into a Linux box:
```bash
dmesg | tail             # expect gspca_ov519 binding
v4l2-ctl --list-devices  # the camera appears
ffplay /dev/video0       # or guvcview / Cheese
```
Success = a stock viewer shows the live 320×240 stream.

This is a vendor-specific device (not UVC), so Windows has no in-box driver;
inspect it there with USB Device Tree Viewer for the descriptors, or
Wireshark+USBPcap to watch the OV519 control traffic and carve out a JPEG.

## Status / notes

- The USB descriptors, OV519 control emulation, and MJPEG framing follow the
  hardware-verified research spec; frames decode on Linux `gspca_ov519`,
  PSCam4Windows, and picojpeg.
- On a real Xbox the device enumerates, completes the OV519/OV7648 handshake, and
  streams a continuous, gap-free MJPEG that the console's OHCI physically receives
  in full (verified via on-console OHCI register/descriptor reads).
- **Known limitation:** the retail *Xbox Video Chat* self-view stays green. Every
  measurable layer of the device matches a real camera, but the app's own in-XBE
  OHCI/usbcamd iso-URB completion doesn't engage for the received stream — an
  app-internal issue not influenceable from the device side. The emulator is
  correct for other consumers (Darkone test harness, Linux gspca, PSCam4Windows).
