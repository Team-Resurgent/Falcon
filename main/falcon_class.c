// Falcon — custom TinyUSB application class driver for the OV519/OV530 camera.
//
// Owns interface 0 (vendor class 0xFF). Standard interface requests (SET/GET
// INTERFACE) arrive here via usbd.c's per-interface routing; the OV519 vendor
// register requests are handled separately in ov519_control.c
// (tud_vendor_control_xfer_cb). Streaming is a single iso IN endpoint (0x81)
// activated per alt-setting; frames come from the MJPEG packetizer.
#include <string.h>
#include "tusb.h"
#include "device/usbd_pvt.h"
#include "esp_log.h"
#include "usb_descriptors.h"
#include "ov519_emu.h"

static const char *TAG = "falcon.cls";

static uint8_t  s_rhport;
static uint8_t  s_alt;                 // current alt-setting of interface 0
static bool     s_streaming;
static uint16_t s_maxpkt;

// Iso IN transfer buffer (one packet). DMA-capable, aligned.
static uint8_t s_iso[FALCON_ISO_MAXPKT_MAX] CFG_TUSB_MEM_ALIGN;

static void submit_next(uint8_t rhport) {
    uint16_t len = 0;
    ov519_stream_next_packet(s_iso, s_maxpkt, &len);
    // Even a zero-length iso packet must be posted to keep the endpoint serviced.
    usbd_edpt_xfer(rhport, FALCON_EP_STREAM_IN, s_iso, len, false);
}

static void start_stream(uint8_t rhport, uint8_t alt) {
    uint8_t epbuf[7];
    falcon_ep_desc_for_alt(alt, epbuf);
    usbd_edpt_iso_activate(rhport, (const tusb_desc_endpoint_t *)epbuf);
    s_maxpkt    = falcon_alt_maxpkt[alt];
    s_streaming = true;
    ov519_stream_reset();
    submit_next(rhport);
    ESP_LOGI(TAG, "stream start: alt %u, maxpkt %u", alt, s_maxpkt);
}

static void stop_stream(uint8_t rhport) {
    if (s_streaming) ESP_LOGI(TAG, "stream stop");
    s_streaming = false;
    usbd_edpt_close(rhport, FALCON_EP_STREAM_IN);
}

// ---- usbd_class_driver_t callbacks ----------------------------------------

static void falcon_init(void) {
    s_alt = 0;
    s_streaming = false;
    ov519_ctrl_reset();
    ov519_stream_reset();
}

static void falcon_reset(uint8_t rhport) {
    (void)rhport;
    s_alt = 0;
    s_streaming = false;
    ov519_ctrl_reset();
    ov519_stream_reset();
}

// Claim interface 0 and reserve the iso FIFO (largest alt). Return the number of
// descriptor bytes this interface occupies (all 5 alt-settings + their EPs).
static uint16_t falcon_open(uint8_t rhport, tusb_desc_interface_t const *itf, uint16_t max_len) {
    if (itf->bInterfaceClass != 0xFF || itf->bInterfaceNumber != 0) return 0;
    s_rhport = rhport;

    usbd_edpt_iso_alloc(rhport, FALCON_EP_STREAM_IN, FALCON_ISO_MAXPKT_MAX);

    // Walk past every alt-setting of interface 0 (and its endpoint) so the core's
    // descriptor parser advances correctly.
    uint16_t drained = 0;
    uint8_t const *p = (uint8_t const *)itf;
    while (drained < max_len) {
        uint8_t blen = p[0], btype = p[1];
        if (blen == 0) break;
        if (btype == TUSB_DESC_INTERFACE &&
            ((tusb_desc_interface_t const *)p)->bInterfaceNumber != 0) break;
        drained += blen;
        p += blen;
    }
    ESP_LOGI(TAG, "open iface0 (%u desc bytes)", drained);
    return drained;
}

static bool falcon_control_xfer(uint8_t rhport, uint8_t stage,
                                tusb_control_request_t const *request) {
    if (stage != CONTROL_STAGE_SETUP) return true;

    if (request->bmRequestType_bit.type == TUSB_REQ_TYPE_STANDARD) {
        switch (request->bRequest) {
            case TUSB_REQ_SET_INTERFACE: {
                uint8_t alt = (uint8_t)request->wValue;
                if (alt >= FALCON_ALT_COUNT) return false;
                s_alt = alt;
                if (alt == 0) stop_stream(rhport);
                else          start_stream(rhport, alt);
                return tud_control_status(rhport, request);
            }
            case TUSB_REQ_GET_INTERFACE: {
                static uint8_t alt; alt = s_alt;
                return tud_control_xfer(rhport, request, &alt, 1);
            }
            default: return false;
        }
    }
    return false;
}

static bool falcon_xfer_cb(uint8_t rhport, uint8_t ep_addr, xfer_result_t result,
                           uint32_t xferred) {
    (void)result; (void)xferred;
    if (ep_addr == FALCON_EP_STREAM_IN && s_streaming) {
        submit_next(rhport);
    }
    return true;
}

static const usbd_class_driver_t s_falcon_driver = {
    .name            = "falcon-cam",
    .init            = falcon_init,
    .deinit          = NULL,
    .reset           = falcon_reset,
    .open            = falcon_open,
    .control_xfer_cb = falcon_control_xfer,
    .xfer_cb         = falcon_xfer_cb,
    .xfer_isr        = NULL,
    .sof             = NULL,
};

usbd_class_driver_t const *usbd_app_driver_get_cb(uint8_t *driver_count) {
    *driver_count = 1;
    return &s_falcon_driver;
}
