// Falcon — OV519 MJPEG packetizer (continuous single-packet generator).
//
// Emits the pre-encoded MJPEG frames as an OV519 iso stream, ONE packet per call.
// Framing (from <scratchpad>/xcam/MJPEG_FRAME_FORMAT.md, matching gspca
// ov519_pkt_scan):
//   * Start-of-frame packet begins  FF FF FF 50  + a 16-byte header, then JPEG.
//   * Body packets carry raw JPEG continuation.
//   * End-of-frame packet begins    FF FF FF 51  (16-byte header); the consumer
//     publishes the accumulated frame and ignores this packet's payload.
//
// One packet per iso transfer (the caller submits the next in its completion
// callback) is what the real camera does and what actually works here: the host
// reads one 768-byte packet per full-speed frame, and each single-packet transfer
// completes SUCCESS. Submitting a whole 12-packet frame as one transfer errored on
// EVERY body (it overran the host's 8-packet iso buffer / the full-speed bus) and
// eventually crashed the controller path.
#include <string.h>
#include "ov519_emu.h"
#include "checkerboard_jpegs.h"

#define OV519_HDR_LEN   16

// NOTE: we used to emit ZERO-LENGTH iso packets between frames to pace ~30fps.
// That backfired in DMA mode: a 0-length iso IN transfer errors PERSISTENTLY on the
// ESP32-S3 dwc2, so the resend-on-error path latched onto one and froze the whole
// stream (ok/bytes stuck, err climbing) -> the app starved and crashed. Zero-length
// pacing packets are removed (set to 0). If frame cadence needs pacing later, do it
// via the SOF interrupt or non-zero filler, NOT 0-length packets.
#define PACE_IDLE_PACKETS  0

enum { PH_SOF = 0, PH_BODY = 1, PH_EOF = 2, PH_IDLE = 3 };

static uint32_t s_frame;     // index into cb_frames[]
static uint32_t s_off;       // byte offset within the current JPEG
static uint8_t  s_phase;     // PH_SOF / PH_BODY / PH_EOF / PH_IDLE
static uint32_t s_idle;      // idle packets remaining before the next frame

static void write_ov519_header(uint8_t *buf, uint8_t marker) {
    // Byte-exact OV519 SOF/EOF header from PCSX2's hardware-proven EyeToy
    // (webcam_handle_data_ov519): all zero EXCEPT byte [0x0A] = the format code,
    // 0x03 for JPEG. The minidriver reads [0x0A] to know the frame is JPEG; our old
    // header left it 0 (and stuffed seq/geometry into 4-9), so the app read frames
    // but never decoded them -> it hung before showing preview.
    for (int i = 0; i < OV519_HDR_LEN; i++) buf[i] = 0;
    buf[0] = 0xFF; buf[1] = 0xFF; buf[2] = 0xFF; buf[3] = marker;   // 0x50 SOF / 0x51 EOF
    buf[0x0A] = 0x03;   // format = JPEG
}

void ov519_stream_reset(void) {
    s_frame = 0;
    s_off   = 0;
    s_phase = PH_SOF;
    s_idle  = 0;
}

// Rewind to this frame's SOF. A frame whose SOF packet never reached the host is
// dropped in its entirety by every known consumer, so when we lose a packet we
// resend the frame from its header instead of continuing mid-frame. (Skipping a
// lost SOF was exactly why the Xbox saw 467 EOFs but only 1 SOF and therefore
// never assembled a single frame -> blank green preview.)
void ov519_stream_restart_frame(void) {
    s_off   = 0;
    s_idle  = 0;
    s_phase = PH_SOF;
}

bool ov519_stream_at_sof(void) {
    return s_phase == PH_SOF || (s_phase == PH_IDLE && s_idle == 0);
}

uint32_t ov519_stream_next_packet(uint8_t *buf, uint16_t maxpkt) {
    if (maxpkt <= OV519_HDR_LEN) return 0;
    const uint8_t *jpg = cb_frames[s_frame].data;
    uint32_t jlen = cb_frames[s_frame].len;

    // Pad the transmitted image to a multiple of 8 bytes so the EOF length field
    // (image bytes / 8) is EXACT, not rounded up. The Xbox KS framer appears to
    // validate accumulated-bytes == length*8; ceil() made length*8 exceed the real
    // byte count on every non-/8 frame -> "incomplete frame" -> error bit -> backoff.
    // Padding rides AFTER the JPEG EOI (FFD9) as zeros, which every decoder ignores.
    uint32_t plen = (jlen + 7u) & ~7u;                       // frame length rounded up to /8

    if (s_phase == PH_SOF) {
        write_ov519_header(buf, 0x50);
        uint32_t room = (uint32_t)maxpkt - OV519_HDR_LEN;   // JPEG bytes in the SOF packet
        uint32_t n = plen < room ? plen : room;
        for (uint32_t i = 0; i < n; i++) buf[OV519_HDR_LEN + i] = (i < jlen) ? jpg[i] : 0x00;
        s_off   = n;
        s_phase = (s_off >= plen) ? PH_EOF : PH_BODY;
        return OV519_HDR_LEN + n;
    }

    if (s_phase == PH_BODY) {
        uint32_t rem = plen - s_off;
        uint32_t n = rem < maxpkt ? rem : maxpkt;           // last body packet is SHORT
        for (uint32_t i = 0; i < n; i++) {
            uint32_t o = s_off + i;
            buf[i] = (o < jlen) ? jpg[o] : 0x00;            // pad past EOI with zeros
        }
        s_off += n;
        if (s_off >= plen) s_phase = PH_EOF;
        return n;
    }

    if (s_phase == PH_IDLE) {
        // Empty inter-frame packet (no marker) — the parser skips it. Pace to ~30fps.
        if (s_idle == 0) { s_phase = PH_SOF; return ov519_stream_next_packet(buf, maxpkt); }
        s_idle--;
        return 0;
    }

    // PH_EOF: header-only publish packet. Per gspca ov519_pkt_scan, the OV519 EOF
    // header carries REAL fields the lenient readers ignore but the Xbox KS parser
    // validates: byte 9 = 0x00 (standard frame WITH image; 0x01 = empty init frame),
    // and bytes 14-15 (LE) = image-data length / 8. We were sending length 0 every
    // frame -> the parser saw a length mismatch and flagged every frame as an error
    // (fail counter -> backoff -> blank green). Fill the real length now.
    uint32_t jlen_done = cb_frames[s_frame].len;   // accumulated frame == the JPEG
    uint32_t plen_done = (jlen_done + 7u) & ~7u;    // we transmitted this many (padded to /8)
    write_ov519_header(buf, 0x51);
    buf[9] = 0x00;                                  // standard frame, has image
    uint32_t l8 = plen_done / 8;                    // EXACT: l8*8 == bytes actually sent
    buf[14] = (uint8_t)l8;
    buf[15] = (uint8_t)(l8 >> 8);
    s_frame = (s_frame + 1) % CHECKERBOARD_FRAME_COUNT;
    s_off   = 0;
    s_idle  = PACE_IDLE_PACKETS;
    s_phase = PH_IDLE;
    return OV519_HDR_LEN;
}
