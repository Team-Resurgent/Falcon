// Falcon — OV519 bridge + OV7648 SCCB control emulation.
//
// The host (gspca_ov519, or the Xbox Video Chat driver) brings the "sensor" up
// by writing/reading OV519 bridge registers over EP0 vendor control transfers
// (bRequest 0x01, wIndex = register). Sensor registers are reached through the
// bridge's SCCB engine (regs 0x42/0x43/0x45/0x47). We don't have a real sensor,
// so we emulate just enough of that engine that the host's detection + init
// sequence succeeds: ACK every write, and return the OV7648 identity bytes on
// the sensor-ID reads (reg 0x0A=0x76, 0x0B=0x48; manufacturer 0x1C=0x7F,
// 0x1D=0xA2). See <scratchpad>/xcam/OV519_OV7648_INIT.md for the real sequence.
//
// TinyUSB routes ALL vendor-type control requests here regardless of recipient
// (usbd.c: `if (type == VENDOR) return tud_vendor_control_xfer_cb(...)`), so the
// OV519 wIndex=register convention does not collide with interface routing.
#include <string.h>
#include "tusb.h"
#include "ov519_emu.h"
#include "esp_log.h"

static const char *TAG = "ov519.ctrl";

#define OV519_BREQ_REG   0x01   // bRequest for bridge register read/write

// Diagnostic counters (surfaced by the heartbeat in wifi_log.c).
volatile uint32_t g_ov_ctrl_rd;    // vendor register reads serviced
volatile uint32_t g_ov_ctrl_wr;    // vendor register writes serviced
volatile uint32_t g_ov_ctrl_other; // non-0x01 vendor requests

static uint8_t s_bridge[256];   // OV519 bridge register file
static uint8_t s_sensor[256];   // OV7648 sensor register file (via SCCB)

static uint8_t s_ep0[64];       // EP0 data-stage scratch (control IN/OUT payload)

// Full power-on register defaults, taken verbatim from PCSX2's hardware-proven
// EyeToy (OV519 + OV7648) emulation (pcsx2/USB/usb-eyetoy). Using the COMPLETE
// register images — not a sparse "just the IDs" set — is what the official Xbox
// Video Chat minidriver needs: it reads bridge reg 0x00 (chip id = 0xC0) and 0x53
// (= 0xFF) during bring-up and rejects the device if they read back 0.
#include "ov519_defaults.h"

void ov519_ctrl_reset(void) {
    memcpy(s_bridge, k_ov519_defaults,  sizeof s_bridge);
    memcpy(s_sensor, k_ov7648_defaults, sizeof s_sensor);
}

// I2C/SCCB register indices on the OV519 bridge.
#define I2C_SADDR_3  0x42   // sub-address for a sensor WRITE
#define I2C_SADDR_2  0x43   // sub-address for a sensor READ
#define I2C_DATA     0x45   // data in/out
#define I2C_CTL_518  0x47   // command (OV518+ path used by the Xbox app)

// Bridge register write — SCCB engine mirrors PCSX2's proven EyeToy emulation
// (webcam_handle_control_eyetoy): a plain register store, with reg 0x47 driving
// the I2C engine. The status/result are ordinary stored registers (no invented
// busy bit): after a read the result lives in reg 0x45, and reg 0x47 reads back
// the last command — which is what the official minidriver verifies.
static void bridge_write(uint8_t reg, uint8_t val) {
    if (reg == I2C_CTL_518) {
        if (val == 0x01) {                          // commit a sensor WRITE
            uint8_t sub = s_bridge[I2C_SADDR_3];
            uint8_t d   = s_bridge[I2C_DATA];
            if (sub == 0x12 && (d & 0x80)) {        // COM7 reset -> reload sensor
                memcpy(s_sensor, k_ov7648_defaults, sizeof s_sensor);
                s_sensor[0x12] = d & ~0x80;
            } else {
                s_sensor[sub] = d;
            }
        } else if (s_bridge[I2C_CTL_518] == 0x03 && val == 0x05) {  // sensor READ
            s_bridge[I2C_DATA] = s_sensor[s_bridge[I2C_SADDR_2]];   // latch into 0x45
        }
    }
    s_bridge[reg] = val;    // store AFTER the check (0x47 read-back = last command)
}

static uint8_t bridge_read(uint8_t reg) {
    return s_bridge[reg];   // every register reads back its stored value (PCSX2 parity)
}

// Weak app hook: TinyUSB calls this for every vendor-type control request.
bool tud_vendor_control_xfer_cb(uint8_t rhport, uint8_t stage,
                                tusb_control_request_t const *request) {
    if (request->bRequest != OV519_BREQ_REG) {
        // Unknown vendor request: ACK a zero-length / short read so we don't stall
        // the host mid-init. (No known OV519 traffic uses other bRequests.)
        if (stage == CONTROL_STAGE_SETUP) {
            g_ov_ctrl_other++;
            ESP_LOGI(TAG, "vendor req bReq=0x%02x type=0x%02x wIndex=0x%04x wLen=%u",
                     request->bRequest, request->bmRequestType, request->wIndex,
                     request->wLength);
            return tud_control_status(rhport, request);
        }
        return true;
    }

    const uint8_t reg = (uint8_t)(request->wIndex & 0xFF);
    const bool is_read = (request->bmRequestType_bit.direction == TUSB_DIR_IN);

    if (is_read) {
        if (stage == CONTROL_STAGE_SETUP) {
            // The official Xbox Video Chat minidriver reads OV519 registers with
            // wLength=8 (Darkone's homebrew used wLength=1). A 1-byte short packet
            // was NOT delivering the value into its 8-byte buffer, so its 0x47 poll
            // never saw "done" and the sensor value never registered — it retried
            // reg 0x0A five times and aborted (STATUS_NOT_SUPPORTED). Return the full
            // requested length, replicating the register value across the buffer so
            // whichever byte offset the driver reads sees it.
            uint8_t val = bridge_read(reg);
            uint16_t len = request->wLength;
            if (len > sizeof(s_ep0)) len = sizeof(s_ep0);
            memset(s_ep0, val, len);
            g_ov_ctrl_rd++;
            // Decode SCCB sensor reads inline: reg 0x45 is the SCCB data register,
            // and s_bridge[0x43] holds the sensor sub-address the app selected. This
            // makes "which sensor register did the app read, and what did we return"
            // legible in one line without cross-referencing the 0x43 writes.
            if (reg == I2C_DATA) {
                ESP_LOGI(TAG, "RD reg=0x45 -> 0x%02x  [SCCB sensor[0x%02x]]  type=0x%02x wLen=%u",
                         s_ep0[0], s_bridge[I2C_SADDR_2], request->bmRequestType, request->wLength);
            } else {
                ESP_LOGI(TAG, "RD reg=0x%02x -> 0x%02x  type=0x%02x wLen=%u",
                         reg, s_ep0[0], request->bmRequestType, request->wLength);
            }
            return tud_control_xfer(rhport, request, s_ep0, len);
        }
        return true;   // DATA/ACK
    } else {
        if (stage == CONTROL_STAGE_SETUP) {
            g_ov_ctrl_wr++;
            if (request->wLength == 0) {                  // some writes are zero-data
                return tud_control_status(rhport, request);
            }
            return tud_control_xfer(rhport, request, s_ep0,
                                    request->wLength > sizeof(s_ep0) ? sizeof(s_ep0)
                                                                     : request->wLength);
        }
        if (stage == CONTROL_STAGE_ACK && request->wLength >= 1) {
            bridge_write(reg, s_ep0[0]);
            // Decode the SCCB read-select (write of the sub-address to 0x43) and the
            // SCCB read/write command (0x47) so the trace shows the app's intent.
            if (reg == I2C_SADDR_2) {
                ESP_LOGI(TAG, "WR reg=0x43 = 0x%02x  [select sensor read reg 0x%02x]", s_ep0[0], s_ep0[0]);
            } else if (reg == I2C_CTL_518) {
                ESP_LOGI(TAG, "WR reg=0x47 = 0x%02x  [SCCB cmd]", s_ep0[0]);
            } else {
                ESP_LOGI(TAG, "WR reg=0x%02x = 0x%02x", reg, s_ep0[0]);
            }
        }
        return true;
    }
}
