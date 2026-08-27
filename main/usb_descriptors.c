// Falcon — USB descriptors, byte-faithful to the Xbox Video Camera's 24x04 EEPROM
// (see <scratchpad>/xcam/EEPROM Descriptor.md). Vendor-specific OV519/OV530 device:
// one vendor-class interface with five iso-IN alt-settings.
#include <string.h>
#include "tusb.h"
#include "usb_descriptors.h"
#include "sdkconfig.h"

// VID/PID: default the official Xbox Video Camera; EyeToy is the Linux-gspca-testable
// alternate (Kconfig FALCON_USB_ID_EYETOY).
#if defined(CONFIG_FALCON_USB_ID_EYETOY)
#  define FALCON_VID 0x054C
#  define FALCON_PID 0x0155
#else
#  define FALCON_VID 0x045E   // Microsoft
#  define FALCON_PID 0x028C   // Xbox Video Camera
#endif

const uint16_t falcon_alt_maxpkt[FALCON_ALT_COUNT] = { 0, 384, 512, 768, 896 };

// ---- Device descriptor (18 bytes) ----------------------------------------
// bcdUSB 1.10, class 0. bMaxPacketSize0 = the compiled EP0 size (pinned to 8 in
// tusb_config.h to match the real camera); tracking CFG_TUD_ENDPOINT0_SIZE keeps
// the descriptor and the stack consistent even if the config is overridden.
static const uint8_t s_device_desc[18] = {
    18, TUSB_DESC_DEVICE, 0x10, 0x01, 0x00, 0x00, 0x00, CFG_TUD_ENDPOINT0_SIZE,
    (uint8_t)(FALCON_VID & 0xFF), (uint8_t)(FALCON_VID >> 8),
    (uint8_t)(FALCON_PID & 0xFF), (uint8_t)(FALCON_PID >> 8),
    0x00, 0x01,          // bcdDevice 1.00
    1, 2, 0,             // iManufacturer, iProduct, iSerial
    1                    // bNumConfigurations
};

// ---- Configuration descriptor (89 bytes, 0x59) ---------------------------
// One interface, 5 alt-settings, each with the iso IN EP 0x81 (alt0 maxpkt 0).
#define EP_ALT(mps) 0x07, 0x05, FALCON_EP_STREAM_IN, 0x01, \
                    (uint8_t)((mps) & 0xFF), (uint8_t)((mps) >> 8), 0x01
#define IFACE_ALT(alt) 0x09, 0x04, 0x00, (alt), 0x01, 0xFF, 0x00, 0x00, 0x00
static const uint8_t s_config_desc[89] = {
    // config: wTotalLength 89, 1 interface, bus-powered, 500 mA
    0x09, TUSB_DESC_CONFIGURATION, 0x59, 0x00, 0x01, 0x01, 0x00, 0x80, 0xFA,
    IFACE_ALT(0), EP_ALT(0),
    IFACE_ALT(1), EP_ALT(384),
    IFACE_ALT(2), EP_ALT(512),
    IFACE_ALT(3), EP_ALT(768),
    IFACE_ALT(4), EP_ALT(896),
};

// ---- Strings -------------------------------------------------------------
static const char *const s_strings[] = {
    (const char[]){ 0x09, 0x04 },   // 0: LangID 0x0409
    "Microsoft",                    // 1: iManufacturer
    "Xbox Video Camera",            // 2: iProduct
};

// esp_tinyusb builds tud_descriptor_*_cb from these (passed to
// tinyusb_driver_install); it ignores any app-provided tud_descriptor_*_cb.
#include "falcon_desc.h"
void falcon_get_descriptors(const tusb_desc_device_t **dev, const uint8_t **cfg,
                            const char ***strs, int *nstr) {
    *dev  = (const tusb_desc_device_t *)s_device_desc;   // same 18-byte layout
    *cfg  = s_config_desc;
    *strs = (const char **)s_strings;
    *nstr = (int)(sizeof(s_strings) / sizeof(s_strings[0]));
}

const void *falcon_ep_desc_for_alt(uint8_t alt, uint8_t out[7]) {
    uint16_t mps = (alt < FALCON_ALT_COUNT) ? falcon_alt_maxpkt[alt] : 0;
    out[0] = 0x07; out[1] = 0x05; out[2] = FALCON_EP_STREAM_IN; out[3] = 0x01;
    out[4] = (uint8_t)(mps & 0xFF); out[5] = (uint8_t)(mps >> 8); out[6] = 0x01;
    return out;
}
