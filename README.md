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

## Verify (PC, no Xbox)

Build with the **EyeToy** identity, plug the **native USB** port into a Linux box:

```bash
dmesg | tail            # expect gspca_ov519 binding
v4l2-ctl --list-devices  # the camera appears
ffplay /dev/video0       # or guvcview / Cheese — see the scrolling checkerboard
```

Success = a stock viewer shows the live 320×240 checkerboard.

## Status / notes

- The MJPEG frame generator is validated (decodes as baseline 320×240).
- The USB descriptors, OV519 control emulation, and MJPEG framing follow the
  hardware-verified research spec.
- **First-flash validation items** (need real hardware): esp_tinyusb picking up
  the custom `tusb_config.h` (EP0 size 8) and the custom class driver; iso IN
  FIFO sizing on the S3; and OV519 SOF/EOF packet alignment against `gspca`.
  Iterate from the COM3 log + `dmesg`.
