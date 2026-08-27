// Falcon — UVC dev-mode streaming: feed the pre-encoded checkerboard MJPEG frames
// to the host through TinyUSB's video class. Adapted from TinyUSB's video_capture
// example (MJPEG path).
#include "tusb.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "uvc_defs.h"
#include "checkerboard_jpegs.h"

static const char *TAG = "falcon.uvc";

static volatile bool     s_tx_busy;
static volatile uint32_t s_interval_ms = 1000 / FALCON_FRAME_RATE;
static uint32_t          s_frame;

// Host committed a format/frame — (re)start streaming at the negotiated rate.
int tud_video_commit_cb(uint_fast8_t ctl_idx, uint_fast8_t stm_idx,
                        video_probe_and_commit_control_t const *parameters) {
    (void)ctl_idx; (void)stm_idx;
    if (parameters->dwFrameInterval) s_interval_ms = parameters->dwFrameInterval / 10000;
    if (s_interval_ms == 0) s_interval_ms = 1;
    s_tx_busy = false;
    s_frame = 0;
    ESP_LOGI(TAG, "commit: streaming %ux%u, interval %u ms",
             FALCON_FRAME_W, FALCON_FRAME_H, (unsigned)s_interval_ms);
    return VIDEO_ERROR_NONE;
}

// One frame finished going out — free to send the next.
void tud_video_frame_xfer_complete_cb(uint_fast8_t ctl_idx, uint_fast8_t stm_idx) {
    (void)ctl_idx; (void)stm_idx;
    s_tx_busy = false;
    s_frame++;
}

static void uvc_task(void *arg) {
    (void)arg;
    for (;;) {
        if (tud_video_n_streaming(0, 0) && !s_tx_busy) {
            uint32_t i = s_frame % CHECKERBOARD_FRAME_COUNT;
            s_tx_busy = true;
            tud_video_n_frame_xfer(0, 0, (void *)cb_frames[i].data, cb_frames[i].len);
        }
        vTaskDelay(pdMS_TO_TICKS(s_interval_ms ? s_interval_ms : 1));
    }
}

void falcon_uvc_start(void) {
    xTaskCreate(uvc_task, "falcon_uvc", 4096, NULL, 4, NULL);
}
