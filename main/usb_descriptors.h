// Falcon — USB descriptors for the Xbox Video Camera (OV519/OV530) emulation.
#pragma once
#include <stdint.h>

// The single iso IN streaming endpoint (EP1 IN), per the parsed camera EEPROM.
#define FALCON_EP_STREAM_IN   0x81

// Interface 0 alt-settings -> iso wMaxPacketSize (bytes). alt0 = zero-bandwidth
// idle/stop; alt3 (768) is the 320x240 mode the retail driver pins.
#define FALCON_ALT_COUNT      5
extern const uint16_t falcon_alt_maxpkt[FALCON_ALT_COUNT];   // {0,384,512,768,896}
#define FALCON_ALT_STREAM_DEFAULT  3    // 320x240

// Largest iso packet across all alts (for usbd_edpt_iso_alloc).
#define FALCON_ISO_MAXPKT_MAX  896

// Build a 7-byte iso IN endpoint descriptor for a given alt into `out` (used by
// usbd_edpt_iso_activate). Returns a pointer to `out`.
const void *falcon_ep_desc_for_alt(uint8_t alt, uint8_t out[7]);
