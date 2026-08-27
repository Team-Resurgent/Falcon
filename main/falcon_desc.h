// Falcon — descriptor accessor shared by both modes. esp_tinyusb builds its
// tud_descriptor_*_cb from the pointers we hand tinyusb_driver_install, so each
// mode exposes its device/config/string descriptors through this one call.
#pragma once
#include "tusb.h"

void falcon_get_descriptors(const tusb_desc_device_t **dev,
                            const uint8_t **cfg,
                            const char ***strs, int *nstr);
