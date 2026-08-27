// Falcon — TinyUSB config. Two build modes (menuconfig -> Falcon camera emulator):
//   * OV519 (default): faithful Xbox Video Camera via a custom application class
//     driver (falcon_class.c); no built-in TinyUSB classes.
//   * UVC dev: a standard UVC MJPEG camera (video class) for PC preview.
#pragma once
#include "sdkconfig.h"

#ifdef __cplusplus
extern "C" {
#endif

// ESP32-S3 native USB-OTG, device mode, full-speed (USB 1.1 — matches the Xbox host).
#ifndef CFG_TUSB_MCU
#define CFG_TUSB_MCU          OPT_MCU_ESP32S3
#endif
#define CFG_TUSB_OS           OPT_OS_FREERTOS
#define CFG_TUSB_RHPORT0_MODE (OPT_MODE_DEVICE | OPT_MODE_FULL_SPEED)

#ifndef CFG_TUSB_MEM_SECTION
#define CFG_TUSB_MEM_SECTION
#endif
#ifndef CFG_TUSB_MEM_ALIGN
#define CFG_TUSB_MEM_ALIGN    __attribute__((aligned(4)))
#endif

#if defined(CONFIG_FALCON_MODE_UVC)
// -------- UVC dev mode: standard MJPEG-over-bulk video camera --------------
#define CFG_TUD_ENDPOINT0_SIZE  64
#define CFG_TUD_VIDEO           1
#define CFG_TUD_VIDEO_STREAMING 1
#define CFG_TUD_VIDEO_STREAMING_BULK        1     // bulk = no iso FIFO tuning, robust on Windows
#define CFG_TUD_VIDEO_STREAMING_EP_BUFSIZE  256
#define CFG_TUD_VIDEO_STREAMING_IF_COUNT    1
#define CFG_TUD_VIDEO_CONTROL_IF_COUNT      1
#define CFG_TUD_CDC     0
#define CFG_TUD_MSC     0
#define CFG_TUD_HID     0
#define CFG_TUD_MIDI    0
#define CFG_TUD_AUDIO   0
#define CFG_TUD_VENDOR  0

#else
// -------- OV519 mode (default): faithful Xbox Video Camera ------------------
// EP0 max packet = 8, byte-faithful to the real camera's device descriptor.
#define CFG_TUD_ENDPOINT0_SIZE  8
#define CFG_TUD_CDC     0
#define CFG_TUD_MSC     0
#define CFG_TUD_HID     0
#define CFG_TUD_MIDI    0
#define CFG_TUD_VIDEO   0
#define CFG_TUD_AUDIO   0
#define CFG_TUD_VENDOR  0   // we implement tud_vendor_control_xfer_cb ourselves (OV519 regs)
#endif

#ifdef __cplusplus
}
#endif
