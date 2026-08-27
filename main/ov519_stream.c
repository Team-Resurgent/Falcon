// Falcon — OV519 MJPEG packetizer.
//
// Emits the pre-encoded checkerboard JPEG frames as an OV519 iso stream. Framing
// (from <scratchpad>/xcam/MJPEG_FRAME_FORMAT.md, matching gspca ov519_pkt_scan):
//   * Start-of-frame packet begins  FF FF FF 50  + a 16-byte header, then JPEG data.
//   * Body packets carry raw JPEG continuation.
//   * End-of-frame packet begins    FF FF FF 51  (16-byte header); the consumer
//     publishes the accumulated frame and ignores this packet's payload.
// The last JPEG bytes therefore ride in the final SOF/body packet, and the EOF
// packet is header-only.
#include <string.h>
#include "ov519_emu.h"
#include "checkerboard_jpegs.h"

#define OV519_HDR_LEN   16

typedef enum { ST_SOF = 0, ST_BODY, ST_EOF } stream_state_t;

static stream_state_t s_state;
static uint32_t s_frame;     // index into cb_frames[]
static uint32_t s_off;       // byte offset within the current JPEG frame
static uint32_t s_seq;       // frame sequence counter (for the header metadata)

static void write_ov519_header(uint8_t *buf, uint8_t marker) {
    buf[0] = 0xFF; buf[1] = 0xFF; buf[2] = 0xFF; buf[3] = marker;   // 0x50 SOF / 0x51 EOF
    // Remaining 12 header bytes are metadata the consumer strips; encode the frame
    // sequence and 320x240 geometry so a capture looks plausible.
    buf[4] = (uint8_t)s_seq; buf[5] = (uint8_t)(s_seq >> 8);
    buf[6] = 0x40; buf[7] = 0x01;   // width 320
    buf[8] = 0xF0; buf[9] = 0x00;   // height 240
    for (int i = 10; i < OV519_HDR_LEN; i++) buf[i] = 0;
}

void ov519_stream_reset(void) {
    s_state = ST_SOF;
    s_frame = 0;
    s_off = 0;
    s_seq = 0;
}

void ov519_stream_next_packet(uint8_t *buf, uint16_t maxpkt, uint16_t *out_len) {
    const uint8_t *jpg = cb_frames[s_frame].data;
    const uint32_t jlen = cb_frames[s_frame].len;

    if (maxpkt == 0) { *out_len = 0; return; }   // zero-bandwidth alt (shouldn't stream)

    switch (s_state) {
        case ST_SOF: {
            write_ov519_header(buf, 0x50);
            uint16_t room = (maxpkt > OV519_HDR_LEN) ? (uint16_t)(maxpkt - OV519_HDR_LEN) : 0;
            uint32_t rem = jlen - s_off;
            uint16_t n = (rem < room) ? (uint16_t)rem : room;
            memcpy(buf + OV519_HDR_LEN, jpg + s_off, n);
            s_off += n;
            *out_len = (uint16_t)(OV519_HDR_LEN + n);
            s_state = (s_off >= jlen) ? ST_EOF : ST_BODY;
            break;
        }
        case ST_BODY: {
            uint32_t rem = jlen - s_off;
            uint16_t n = (rem < maxpkt) ? (uint16_t)rem : maxpkt;
            memcpy(buf, jpg + s_off, n);
            s_off += n;
            *out_len = n;
            if (s_off >= jlen) s_state = ST_EOF;
            break;
        }
        case ST_EOF:
        default: {
            write_ov519_header(buf, 0x51);
            *out_len = OV519_HDR_LEN;
            // Advance to the next frame (loop the checkerboard set).
            s_seq++;
            s_frame = (s_frame + 1) % CHECKERBOARD_FRAME_COUNT;
            s_off = 0;
            s_state = ST_SOF;
            break;
        }
    }
}
