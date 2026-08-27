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
#include "tusb.h"
#include "ov519_emu.h"
#include "esp_log.h"

static const char *TAG = "ov519.ctrl";

#define OV519_BREQ_REG   0x01   // bRequest for bridge register read/write

static uint8_t s_bridge[256];   // OV519 bridge register file
static uint8_t s_sensor[256];   // OV7648 sensor register file (via SCCB)

static uint8_t s_wsub;          // pending sensor write sub-address (bridge reg 0x42)
static uint8_t s_rsub;          // pending sensor read  sub-address (bridge reg 0x43)
static uint8_t s_sdata;         // pending sensor write data        (bridge reg 0x45)
static uint8_t s_sread;         // last sensor read result (returned via bridge reg 0x45)

static uint8_t s_ep0[8];        // EP0 data-stage scratch (control OUT payload)

void ov519_ctrl_reset(void) {
    for (int i = 0; i < 256; i++) { s_bridge[i] = 0; s_sensor[i] = 0; }
    // OV7648 identity so the host's detect/ident passes.
    s_sensor[0x1C] = 0x7F;   // manufacturer id high
    s_sensor[0x1D] = 0xA2;   // manufacturer id low
    s_sensor[0x0A] = 0x76;   // product id high  -> OV76xx
    s_sensor[0x0B] = 0x48;   // product id low   -> ...48 (OV7648)
    s_wsub = s_rsub = s_sdata = s_sread = 0;
}

// Apply a bridge register write, driving the SCCB engine for sensor access.
static void bridge_write(uint8_t reg, uint8_t val) {
    s_bridge[reg] = val;
    switch (reg) {
        case 0x42: s_wsub  = val; break;                 // sensor write sub-address
        case 0x43: s_rsub  = val; break;                 // sensor read  sub-address
        case 0x45: s_sdata = val; break;                 // sensor data (for a write)
        case 0x47:                                       // SCCB command
            if (val == 0x01) {                           // initiate write
                s_sensor[s_wsub] = s_sdata;
            } else if (val == 0x03 || val == 0x05) {     // read phases
                s_sread = s_sensor[s_rsub];              // latch result for reg 0x45
            }
            break;
        default: break;
    }
}

static uint8_t bridge_read(uint8_t reg) {
    switch (reg) {
        case 0x45: return s_sread;      // SCCB read result
        case 0x47: return 0x00;         // status: bit0=0 -> "not busy" so host poll loops exit
        default:   return s_bridge[reg];
    }
}

// Weak app hook: TinyUSB calls this for every vendor-type control request.
bool tud_vendor_control_xfer_cb(uint8_t rhport, uint8_t stage,
                                tusb_control_request_t const *request) {
    if (request->bRequest != OV519_BREQ_REG) {
        // Unknown vendor request: ACK a zero-length / short read so we don't stall
        // the host mid-init. (No known OV519 traffic uses other bRequests.)
        if (stage == CONTROL_STAGE_SETUP)
            return tud_control_status(rhport, request);
        return true;
    }

    const uint8_t reg = (uint8_t)(request->wIndex & 0xFF);
    const bool is_read = (request->bmRequestType_bit.direction == TUSB_DIR_IN);

    if (is_read) {
        if (stage == CONTROL_STAGE_SETUP) {
            s_ep0[0] = bridge_read(reg);
            uint16_t len = request->wLength ? 1 : 0;
            return tud_control_xfer(rhport, request, s_ep0, len);
        }
        return true;   // DATA/ACK
    } else {
        if (stage == CONTROL_STAGE_SETUP) {
            if (request->wLength == 0) {                  // some writes are zero-data
                return tud_control_status(rhport, request);
            }
            return tud_control_xfer(rhport, request, s_ep0,
                                    request->wLength > sizeof(s_ep0) ? sizeof(s_ep0)
                                                                     : request->wLength);
        }
        if (stage == CONTROL_STAGE_ACK && request->wLength >= 1) {
            bridge_write(reg, s_ep0[0]);
        }
        return true;
    }
}
