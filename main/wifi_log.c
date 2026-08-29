// Falcon — WiFi UDP debug logging (see wifi_log.h).
// Compiled only when CONFIG_FALCON_WIFI_LOG is set (Kconfig: FALCON_WIFI_LOG); the
// build system excludes this file otherwise, so wifi_creds.h is only ever needed
// with the feature explicitly enabled.
#include "sdkconfig.h"
#if defined(CONFIG_FALCON_WIFI_LOG)
#include <string.h>
#include <stdio.h>
#include "wifi_log.h"
#include "wifi_creds.h"
#include "tusb.h"
#include "esp_log.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "esp_mac.h"
#include "nvs_flash.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lwip/sockets.h"

#define FALCON_LOG_PORT 5555

static const char *TAG = "falcon.wifi";

// Diagnostic counters defined in falcon_class.c / ov519_control.c.
extern volatile uint32_t g_falcon_pkts, g_falcon_bytes, g_falcon_xfer_ok,
                         g_falcon_xfer_err, g_falcon_submit_fail,
                         g_falcon_open, g_falcon_reset, g_falcon_setintf;
extern volatile uint8_t  g_falcon_last_result;
extern volatile uint32_t g_ov_ctrl_rd, g_ov_ctrl_wr, g_ov_ctrl_other;
void falcon_get_stats(uint8_t *alt, uint8_t *streaming);

static int              s_sock = -1;
static struct sockaddr_in s_bcast;
static vprintf_like_t   s_orig_vprintf;
static volatile bool    s_net_up;

// esp_log hook: keep UART output, and when the socket is up also UDP-broadcast it.
static int log_vprintf(const char *fmt, va_list ap) {
    if (s_sock >= 0 && s_net_up) {
        char buf[256];
        va_list ap2; va_copy(ap2, ap);
        int n = vsnprintf(buf, sizeof(buf), fmt, ap2);
        va_end(ap2);
        if (n > 0) {
            if (n > (int)sizeof(buf)) n = sizeof(buf);
            sendto(s_sock, buf, n, 0, (struct sockaddr *)&s_bcast, sizeof(s_bcast));
        }
    }
    return s_orig_vprintf ? s_orig_vprintf(fmt, ap) : 0;
}

static void on_wifi(void *arg, esp_event_base_t base, int32_t id, void *data) {
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_START) esp_wifi_connect();
    else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        s_net_up = false;
        esp_wifi_connect();
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *e = (ip_event_got_ip_t *)data;
        if (s_sock < 0) {
            s_sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
            int yes = 1;
            setsockopt(s_sock, SOL_SOCKET, SO_BROADCAST, &yes, sizeof(yes));
            memset(&s_bcast, 0, sizeof(s_bcast));
            s_bcast.sin_family = AF_INET;
            s_bcast.sin_port = htons(FALCON_LOG_PORT);
            s_bcast.sin_addr.s_addr = htonl(INADDR_BROADCAST); // 255.255.255.255
        }
        s_net_up = true;
        ESP_LOGI(TAG, "wifi up, ip=" IPSTR ", broadcasting logs on udp/%d",
                 IP2STR(&e->ip_info.ip), FALCON_LOG_PORT);
    }
}

static void heartbeat_task(void *arg) {
    uint32_t last_pkts = 0;
    for (uint32_t t = 0;; t++) {
        vTaskDelay(pdMS_TO_TICKS(1000));
        uint8_t alt = 0, str = 0;
        falcon_get_stats(&alt, &str);
        uint32_t pkts = g_falcon_pkts;
        ESP_LOGI(TAG, "HB t=%us rst=%lu open=%lu cfgd=%u setintf=%lu alt=%u str=%u "
                      "ctrlR=%lu ctrlW=%lu ctrlO=%lu pkts=%lu (+%lu/s) ok=%lu err=%lu "
                      "sfail=%lu lastres=%u free=%u",
                 t, (unsigned long)g_falcon_reset, (unsigned long)g_falcon_open,
                 tud_mounted() ? 1 : 0, (unsigned long)g_falcon_setintf, alt, str,
                 (unsigned long)g_ov_ctrl_rd, (unsigned long)g_ov_ctrl_wr,
                 (unsigned long)g_ov_ctrl_other, (unsigned long)pkts,
                 (unsigned long)(pkts - last_pkts),
                 (unsigned long)g_falcon_xfer_ok, (unsigned long)g_falcon_xfer_err,
                 (unsigned long)g_falcon_submit_fail, g_falcon_last_result,
                 (unsigned)esp_get_free_heap_size());
        last_pkts = pkts;
    }
}

void wifi_log_start(void) {
    s_orig_vprintf = esp_log_set_vprintf(log_vprintf);

    esp_err_t e = nvs_flash_init();
    if (e == ESP_ERR_NVS_NO_FREE_PAGES || e == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        nvs_flash_erase(); nvs_flash_init();
    }
    esp_netif_init();
    esp_event_loop_create_default();
    esp_netif_create_default_wifi_sta();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    esp_wifi_init(&cfg);
    esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID, on_wifi, NULL, NULL);
    esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP, on_wifi, NULL, NULL);

    wifi_config_t wc = {0};
    strncpy((char *)wc.sta.ssid, FALCON_WIFI_SSID, sizeof(wc.sta.ssid));
    strncpy((char *)wc.sta.password, FALCON_WIFI_PASS, sizeof(wc.sta.password));
    esp_wifi_set_mode(WIFI_MODE_STA);
    esp_wifi_set_config(WIFI_IF_STA, &wc);
    esp_wifi_set_ps(WIFI_PS_NONE);   // no power-save: keep logs flowing promptly
    esp_wifi_start();

    xTaskCreate(heartbeat_task, "falcon_hb", 3072, NULL, 3, NULL);
    ESP_LOGI(TAG, "wifi log starting, ssid=%s", FALCON_WIFI_SSID);
}

#endif // CONFIG_FALCON_WIFI_LOG
