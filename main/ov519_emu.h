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

// Rewind to the start (SOF) of the CURRENT frame. Used when a packet is lost:
// a frame whose SOF never reached the host is discarded whole by every consumer
// (gspca, PSCam4Windows and the Xbox KS framer all drop data until they see
// FF FF FF 50), so on any packet loss we restart the frame rather than emit a
// headerless fragment.
void ov519_stream_restart_frame(void);

// True while the next packet to send is the frame's SOF (must never be skipped).
bool ov519_stream_at_sof(void);

// Fill ONE iso packet (<= maxpkt bytes) of the continuous OV519 stream into `buf`
// and return its length. Successive calls walk a state machine per frame:
//   * SOF packet:  FF FF FF 50 + 16B header, then up to (maxpkt-16) JPEG bytes.
//   * body packets: maxpkt JPEG bytes each; the final one is SHORT (real length),
//     so the consumer's per-packet BytesRead ends exactly at the JPEG EOI.
//   * EOF packet:  FF FF FF 51 + 16B header (16 bytes) -> publish, advance frame.
// Feeding ONE packet per iso transfer (submit the next in the completion callback)
// matches the real camera's continuous stream and never overruns the full-speed
// bus: unlike a multi-packet whole-frame transfer, each single-packet transfer
// completes cleanly (the whole-frame transfers errored on every body).
uint32_t ov519_stream_next_packet(uint8_t *buf, uint16_t maxpkt);
