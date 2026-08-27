// Falcon — ESP32-S3 emulator of the original Xbox Video Camera (OmniVision
// OV530 / OV519), so an unmodified Xbox Video Chat app (or Linux gspca_ov519)
// sees a real camera. Milestone 1: streams a scrolling-checkerboard MJPEG.
//
// The native USB-OTG port is the camera; console/flash is on UART0 (COM3).
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "tinyusb.h"
#include "sdkconfig.h"
#include "falcon_desc.h"
#if defined(CONFIG_FALCON_MODE_UVC)
#include "uvc_defs.h"
#endif

static const char *TAG = "falcon";

void app_main(void) {
#if defined(CONFIG_FALCON_MODE_UVC)
    ESP_LOGI(TAG, "Falcon: UVC dev camera starting");
#else
    ESP_LOGI(TAG, "Falcon: Xbox Video Camera (OV519/OV530) emulator starting");
#endif

    // esp_tinyusb builds its tud_descriptor_*_cb from the pointers we pass here
    // (passing NULL makes it use ITS OWN default descriptors, not our callbacks).
    // The OV519 mode additionally registers a custom class driver via
    // usbd_app_driver_get_cb (falcon_class.c).
    const tusb_desc_device_t *dev; const uint8_t *cfg; const char **strs; int nstr;
    falcon_get_descriptors(&dev, &cfg, &strs, &nstr);
    const tinyusb_config_t tusb_cfg = {
        .device_descriptor        = dev,
        .string_descriptor        = strs,
        .string_descriptor_count  = nstr,
        .configuration_descriptor = cfg,
        .external_phy             = false,
    };
    ESP_ERROR_CHECK(tinyusb_driver_install(&tusb_cfg));

#if defined(CONFIG_FALCON_MODE_UVC)
    falcon_uvc_start();   // spawn the MJPEG frame feeder
    ESP_LOGI(TAG, "USB UVC device installed — open it in any camera app");
#else
    ESP_LOGI(TAG, "USB device installed — streaming starts on SET_INTERFACE(alt>=1)");
#endif

    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(5000));
    }
}
