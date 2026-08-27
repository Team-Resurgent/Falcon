// Falcon — ESP32-S3 emulator of the original Xbox Video Camera (OmniVision
// OV530 / OV519), so an unmodified Xbox Video Chat app (or Linux gspca_ov519)
// sees a real camera. Milestone 1: streams a scrolling-checkerboard MJPEG.
//
// The native USB-OTG port is the camera; console/flash is on UART0 (COM3).
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "tinyusb.h"

static const char *TAG = "falcon";

void app_main(void) {
    ESP_LOGI(TAG, "Falcon: Xbox Video Camera (OV519/OV530) emulator starting");

    // Descriptors come from our tud_descriptor_*_cb (usb_descriptors.c); the
    // custom OV519 class driver is registered via usbd_app_driver_get_cb
    // (falcon_class.c). Pass NULLs so esp_tinyusb uses the weak callbacks.
    const tinyusb_config_t tusb_cfg = {
        .device_descriptor        = NULL,
        .string_descriptor        = NULL,
        .external_phy             = false,
        .configuration_descriptor = NULL,
    };
    ESP_ERROR_CHECK(tinyusb_driver_install(&tusb_cfg));
    ESP_LOGI(TAG, "USB device installed — streaming starts on SET_INTERFACE(alt>=1)");

    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(5000));
    }
}
