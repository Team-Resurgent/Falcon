// Falcon — shared interface between the OV519 control emulation, the MJPEG
// packetizer, and the custom USB class driver.
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// --- OV519 register / SCCB control emulation (ov519_control.c) --------------
// Reset all emulated bridge + sensor state (called on USB reset / driver init).
// Presets the OV7648 detection IDs so the host's sensor probe succeeds.
void ov519_ctrl_reset(void);

// --- MJPEG packetizer (ov519_stream.c) -------------------------------------
// Reset the frame/packet cursor to the start of a fresh frame.
void ov519_stream_reset(void);

// Produce the next iso packet (OV519-framed MJPEG) into `buf` (capacity =
// current alt's wMaxPacketSize). Writes the packet length to *out_len
// (0 <= *out_len <= maxpkt). Never blocks.
void ov519_stream_next_packet(uint8_t *buf, uint16_t maxpkt, uint16_t *out_len);
