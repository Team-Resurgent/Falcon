// Falcon — custom TinyUSB application class driver for the OV519/OV530 camera.
//
// Owns interface 0 (vendor class 0xFF). Standard interface requests (SET/GET
// INTERFACE) arrive here via usbd.c's per-interface routing; the OV519 vendor
// register requests are handled separately in ov519_control.c
// (tud_vendor_control_xfer_cb). Streaming is a single iso IN endpoint (0x81)
// activated per alt-setting; frames come from the MJPEG packetizer.
#include <string.h>
#include "tusb.h"
#include "device/usbd.h"
#include "device/usbd_pvt.h"
#include "esp_log.h"
#include "usb_descriptors.h"
#include "ov519_emu.h"

static const char *TAG = "falcon.cls";

static uint8_t  s_rhport;
static uint8_t  s_alt;                 // current alt-setting of interface 0
static bool     s_streaming;
static uint16_t s_maxpkt;

// Diagnostic counters (read by the heartbeat logger in falcon_main.c).
volatile uint32_t g_falcon_pkts;       // iso packets posted
volatile uint32_t g_falcon_bytes;      // iso bytes posted
volatile uint32_t g_falcon_xfer_ok;    // iso completions with SUCCESS
volatile uint32_t g_falcon_xfer_err;   // iso completions with non-SUCCESS
volatile uint32_t g_falcon_submit_fail;// usbd_edpt_xfer returned false
volatile uint32_t g_falcon_body_err;   // non-SUCCESS completions on a frame-body xfer
volatile uint32_t g_falcon_eof_err;    // non-SUCCESS completions on an EOF xfer
volatile uint8_t  g_falcon_last_result;
volatile uint32_t g_falcon_open;       // times the host configured/claimed iface0
volatile uint32_t g_falcon_reset;      // bus resets seen
volatile uint32_t g_falcon_setintf;    // SET_INTERFACE requests

void falcon_get_stats(uint8_t *alt, uint8_t *streaming) {
    if (alt) *alt = s_alt;
    if (streaming) *streaming = s_streaming ? 1 : 0;
}

// Single-packet iso feeding. We keep exactly ONE <=maxpkt packet in flight and
// submit the next in the completion callback. This matches the real camera's
// continuous stream and the full-speed bus (one 768-byte packet per frame) and,
// unlike a whole-frame multi-packet transfer, every transfer completes SUCCESS.
// Double-buffered so the DCD's in-flight buffer is never the one we refill.
static uint8_t s_pktbuf[2][FALCON_ISO_MAXPKT_MAX] CFG_TUSB_MEM_ALIGN;
static uint8_t s_pkt_idx;
static uint8_t *s_cur_buf;              // packet currently in flight
static uint32_t s_cur_len;
static uint32_t s_resend;               // consecutive resends of the current packet
static volatile bool s_inflight;        // a transfer is armed and not yet completed
static volatile bool s_last_err;        // last completed transfer errored (retry it)
static volatile bool s_cur_is_sof;      // in-flight packet is a frame's SOF (never skip)
#define FALCON_ISO_RESEND_MAX 12        // cap so a bad packet can't freeze the stream
#define FALCON_ISO_SEND_CAP   768       // full alt3 packet (matches real camera); FIFO fits it

// Generate + submit the NEXT packet in the stream (advances the packetizer).
static void submit_next(uint8_t rhport) {
    uint8_t *buf = s_pktbuf[s_pkt_idx];
    s_pkt_idx ^= 1;
    s_cur_buf = buf;
    // Cap the on-wire packet size below the full-speed-iso size where this S3 starts
    // dropping large packets (768-byte SOF/body packets were lost on the wire while
    // <=616B packets always arrived). The descriptor still advertises maxpkt; sending
    // short iso packets is legal and every consumer handles it.
    uint16_t sendmax = s_maxpkt > FALCON_ISO_SEND_CAP ? FALCON_ISO_SEND_CAP : s_maxpkt;
    s_cur_is_sof = ov519_stream_at_sof();   // note BEFORE generating (it advances)
    s_cur_len = ov519_stream_next_packet(buf, sendmax);
    if (s_cur_is_sof) g_falcon_body_err++;  // DIAG: count SOF packets SUBMITTED
    bool ok = usbd_edpt_xfer(rhport, FALCON_EP_STREAM_IN, buf, s_cur_len, false);
    if (ok) { g_falcon_pkts++; g_falcon_bytes += s_cur_len; s_inflight = true; }
    else    { g_falcon_submit_fail++; s_inflight = false; }
}

// Re-submit the SAME packet (used after a failed iso transfer). A dropped iso IN
// packet must be retried, NOT skipped: skipping loses bytes out of the middle of
// the MJPEG frame, so the reassembled JPEG is corrupt and the Xbox KS framer flags
// EVERY frame as an error (fail counter -> 500-tick backoff -> blank green). The
// data still lives in s_cur_buf (double-buffered; the next packet uses the other
// slot), so we can resend it verbatim until it gets through.
static void resubmit_cur(uint8_t rhport) {
    bool ok = usbd_edpt_xfer(rhport, FALCON_EP_STREAM_IN, s_cur_buf, s_cur_len, false);
    if (ok) { g_falcon_pkts++; g_falcon_bytes += s_cur_len; s_inflight = true; }
    else    { g_falcon_submit_fail++; s_inflight = false; }
}

// Decide + arm the next iso transfer: resend the current packet after an error
// (never skipping a SOF; mid-frame retries are capped, then the frame restarts),
// otherwise advance to the next packet. submit_next/resubmit_cur set s_inflight to
// the true xfer result, so a failed submit leaves s_inflight=false for the SOF
// watchdog to retry.
static void falcon_arm_next(uint8_t rhport) {
    if (s_inflight) return;
    if (s_last_err && s_cur_len != 0) {
        if (s_cur_is_sof || ++s_resend <= FALCON_ISO_RESEND_MAX) {
            resubmit_cur(rhport);
            return;
        }
        s_resend = 0;
        ov519_stream_restart_frame();
    } else {
        s_resend = 0;
    }
    submit_next(rhport);
}

static void start_stream(uint8_t rhport, uint8_t alt) {
    uint8_t epbuf[7];
    falcon_ep_desc_for_alt(alt, epbuf);
    usbd_edpt_iso_activate(rhport, (const tusb_desc_endpoint_t *)epbuf);
    s_maxpkt    = falcon_alt_maxpkt[alt];
    s_streaming = true;
    s_pkt_idx   = 0;
    s_inflight  = false;
    s_last_err  = false;
    s_resend    = 0;
    ov519_stream_reset();
    // CONTINUOUS delivery: prime the first packet now and re-arm the next one
    // immediately in the completion callback (falcon_xfer_cb), so a packet is ready
    // for EVERY host poll — matching a real camera's gap-free stream. (SOF-paced
    // arming gated on s_inflight only filled ~every-other frame: when a completion
    // landed after the next SOF, that frame was skipped, so the Xbox OHCI saw
    // empty/CC=9 mid-JPEG packets and its usbcamd URB never completed -> no frame.)
    falcon_arm_next(rhport);
    // SOF now only acts as a watchdog (re-arm if a submit failed / completion missed).
    usbd_sof_enable(rhport, SOF_CONSUMER_USER, true);
    ESP_LOGI(TAG, "stream start: alt %u, maxpkt %u (continuous)", alt, s_maxpkt);
}

static void stop_stream(uint8_t rhport) {
    if (s_streaming) ESP_LOGI(TAG, "stream stop");
    s_streaming = false;
    s_inflight  = false;
    usbd_sof_enable(rhport, SOF_CONSUMER_USER, false);
    usbd_edpt_close(rhport, FALCON_EP_STREAM_IN);
}

// SOF watchdog (ISR, once per USB frame). Completion-driven arming in falcon_xfer_cb
// is the primary pump (continuous delivery); this only re-arms when nothing is in
// flight — i.e. a submit failed or a completion was missed — so the stream can't stall.
static void falcon_sof(uint8_t rhport, uint32_t frame_count) {
    (void)frame_count;
    if (!s_streaming) return;
    falcon_arm_next(rhport);
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
    g_falcon_reset++;
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

    // The ESP32-S3 FS PHY has only 256 words (1 KB) of total device FIFO. Allocating
    // for 896 (224 words) over-budgets it (RX + EP0 + DMA reserve leave ~206 words),
    // so the iso TX FIFO ends up broken/undersized -> in DMA mode the engine can't
    // prefill a large packet before the host polls and it truncates. Allocate a size
    // that FITS with headroom (800B = 200 words), and cap the on-wire packet below it.
    usbd_edpt_iso_alloc(rhport, FALCON_EP_STREAM_IN, 800);

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
    g_falcon_open++;
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
                g_falcon_setintf++;
                ESP_LOGI(TAG, "SET_INTERFACE alt=%u", alt);
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
    (void)xferred;
    if (ep_addr == FALCON_EP_STREAM_IN) {
        g_falcon_last_result = (uint8_t)result;
        s_inflight = false;
        if (result == XFER_RESULT_SUCCESS) {
            g_falcon_xfer_ok++;  s_last_err = false;
            if (s_cur_is_sof) g_falcon_eof_err++;   // DIAG: SOF packets COMPLETED ok
        } else {
            g_falcon_xfer_err++; s_last_err = true;
        }
        // CONTINUOUS delivery: arm the next packet right now so the host has data on
        // the very next frame (no every-other-frame gap). The real camera streams
        // gap-free; the Xbox usbcamd URB-completion needs that to actually complete a
        // frame. SOF is only a fallback watchdog. (Earlier "continuous on completion"
        // notes predate the 800-FIFO + EP0-DMA fixes that made DMA iso deliver cleanly.)
        if (s_streaming) falcon_arm_next(rhport);
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
    .sof             = falcon_sof,
};

usbd_class_driver_t const *usbd_app_driver_get_cb(uint8_t *driver_count) {
    *driver_count = 1;
    return &s_falcon_driver;
}
