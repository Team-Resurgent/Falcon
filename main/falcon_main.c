// Falcon — ESP32-S3 emulator of the original Xbox Video Camera (OmniVision
// OV530 / OV519), so an unmodified Xbox Video Chat app (or Linux gspca_ov519)
// sees a real camera. Streams a 320x240 MJPEG over the vendor iso endpoint.
//
// The native USB-OTG port is the camera; console/flash is on UART0 (COM3).
#include "esp_log.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "tinyusb.h"
#include "sdkconfig.h"
#include "falcon_desc.h"
#if defined(CONFIG_FALCON_WIFI_LOG)
#include "wifi_log.h"
#endif

static const char *TAG = "falcon";

void app_main(void) {
    // WiFi logging is OFF by default (Kconfig: FALCON_WIFI_LOG). On the Xbox's
    // timing-strict USB stack the radio's CPU/interrupt/current load breaks
    // enumeration (repeated bus resets, never configured), so enable it only for
    // PC-side debugging where it's harmless -- and provide main/wifi_creds.h
    // (copy main/wifi_creds.h.example). It is compiled in only when enabled.
    #if defined(CONFIG_FALCON_WIFI_LOG)
    wifi_log_start();
    #endif
    ESP_LOGI(TAG, "boot: reset_reason=%d", (int)esp_reset_reason());
    ESP_LOGI(TAG, "Falcon: Xbox Video Camera (OV519/OV530) emulator starting");

    // esp_tinyusb builds its tud_descriptor_*_cb from the pointers we pass here
    // (passing NULL makes it use ITS OWN default descriptors, not our callbacks).
    // We additionally register a custom application class driver via
    // usbd_app_driver_get_cb (falcon_class.c) to own the vendor interface + iso EP.
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
    ESP_LOGI(TAG, "USB device installed — streaming starts on SET_INTERFACE(alt>=1)");

    // Periodic UART status so a serial monitor sees the whole negotiation even if
    // single event lines are missed — and so a reboot is obvious (t resets).
    extern volatile uint32_t g_falcon_reset, g_falcon_open, g_falcon_setintf,
                             g_falcon_pkts, g_falcon_bytes, g_falcon_xfer_ok,
                             g_falcon_xfer_err, g_falcon_submit_fail,
                             g_falcon_body_err, g_falcon_eof_err;
    extern volatile uint32_t g_ov_ctrl_rd, g_ov_ctrl_wr, g_ov_ctrl_other;
    for (uint32_t t = 0;; t++) {
        vTaskDelay(pdMS_TO_TICKS(2000));
        ESP_LOGI(TAG, "HB t=%us mnt=%d rst=%lu open=%lu setintf=%lu ctrlR=%lu "
                      "ctrlW=%lu ctrlO=%lu pkts=%lu bytes=%lu ok=%lu err=%lu(b%lu/e%lu) sfail=%lu",
                 t * 2, tud_mounted() ? 1 : 0, (unsigned long)g_falcon_reset,
                 (unsigned long)g_falcon_open, (unsigned long)g_falcon_setintf,
                 (unsigned long)g_ov_ctrl_rd, (unsigned long)g_ov_ctrl_wr,
                 (unsigned long)g_ov_ctrl_other, (unsigned long)g_falcon_pkts,
                 (unsigned long)g_falcon_bytes, (unsigned long)g_falcon_xfer_ok,
                 (unsigned long)g_falcon_xfer_err, (unsigned long)g_falcon_body_err,
                 (unsigned long)g_falcon_eof_err, (unsigned long)g_falcon_submit_fail);
    }
}
