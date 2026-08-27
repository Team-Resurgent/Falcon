// Falcon — UVC "dev mode" descriptors (CONFIG_FALCON_MODE_UVC).
//
// A standard USB Video Class MJPEG camera so a stock host (Windows Camera app,
// OBS, ffplay) shows the checkerboard with no driver. This is NOT the faithful
// Xbox camera (that is the OV519 mode); it exists purely to bring up + preview
// the ESP32 streaming pipeline on a PC. MJPEG over BULK — robust on Windows and
// free of isochronous FIFO tuning.
//
// Adapted from TinyUSB's examples/device/video_capture (MJPEG + bulk path).
#include <string.h>
#include "tusb.h"
#include "uvc_defs.h"

#define UVC_VID  0x1209   // pid.codes (generic/community)
#define UVC_PID  0xF01C   // "Falcon" dev UVC
#define UVC_BCD  0x0200

enum { STRID_LANGID = 0, STRID_MANUFACTURER, STRID_PRODUCT, STRID_UVC_CONTROL, STRID_UVC_STREAMING };
enum { ITF_NUM_VIDEO_CONTROL = 0, ITF_NUM_VIDEO_STREAMING, ITF_NUM_TOTAL };

#define UVC_CLOCK_FREQUENCY            27000000
#define UVC_ENTITY_CAP_INPUT_TERMINAL  0x01
#define UVC_ENTITY_CAP_OUTPUT_TERMINAL 0x02
#define EPNUM_VIDEO_IN                 0x81

// ---- Device descriptor (IAD / MISC so the host groups VC+VS as one camera) --
static const tusb_desc_device_t s_desc_device = {
    .bLength = sizeof(tusb_desc_device_t), .bDescriptorType = TUSB_DESC_DEVICE,
    .bcdUSB = UVC_BCD,
    .bDeviceClass = TUSB_CLASS_MISC, .bDeviceSubClass = MISC_SUBCLASS_COMMON,
    .bDeviceProtocol = MISC_PROTOCOL_IAD,
    .bMaxPacketSize0 = CFG_TUD_ENDPOINT0_SIZE,
    .idVendor = UVC_VID, .idProduct = UVC_PID, .bcdDevice = 0x0100,
    .iManufacturer = STRID_MANUFACTURER, .iProduct = STRID_PRODUCT, .iSerialNumber = 0,
    .bNumConfigurations = 1,
};

const uint8_t *tud_descriptor_device_cb(void) { return (const uint8_t *)&s_desc_device; }

// ---- Configuration descriptor (MJPEG, bulk streaming) --------------------
typedef struct TU_ATTR_PACKED {
    tusb_desc_interface_t itf;
    tusb_desc_video_control_header_1itf_t header;
    tusb_desc_video_control_camera_terminal_t camera_terminal;
    tusb_desc_video_control_output_terminal_t output_terminal;
} uvc_control_desc_t;

typedef struct TU_ATTR_PACKED {
    tusb_desc_interface_t itf;
    tusb_desc_video_streaming_input_header_1byte_t header;
    tusb_desc_video_format_mjpeg_t format;
    tusb_desc_video_frame_mjpeg_continuous_t frame;
    tusb_desc_video_streaming_color_matching_t color;
    tusb_desc_endpoint_t ep;   // bulk IN (no iso alt-setting needed)
} uvc_streaming_desc_t;

typedef struct TU_ATTR_PACKED {
    tusb_desc_configuration_t config;
    tusb_desc_interface_assoc_t iad;
    uvc_control_desc_t vc;
    uvc_streaming_desc_t vs;
} uvc_cfg_desc_t;

static const uvc_cfg_desc_t s_desc_cfg = {
    .config = {
        .bLength = sizeof(tusb_desc_configuration_t), .bDescriptorType = TUSB_DESC_CONFIGURATION,
        .wTotalLength = sizeof(uvc_cfg_desc_t), .bNumInterfaces = ITF_NUM_TOTAL,
        .bConfigurationValue = 1, .iConfiguration = 0, .bmAttributes = TU_BIT(7), .bMaxPower = 250,
    },
    .iad = {
        .bLength = sizeof(tusb_desc_interface_assoc_t), .bDescriptorType = TUSB_DESC_INTERFACE_ASSOCIATION,
        .bFirstInterface = ITF_NUM_VIDEO_CONTROL, .bInterfaceCount = 2,
        .bFunctionClass = TUSB_CLASS_VIDEO, .bFunctionSubClass = VIDEO_SUBCLASS_INTERFACE_COLLECTION,
        .bFunctionProtocol = VIDEO_ITF_PROTOCOL_UNDEFINED, .iFunction = 0,
    },
    .vc = {
        .itf = {
            .bLength = sizeof(tusb_desc_interface_t), .bDescriptorType = TUSB_DESC_INTERFACE,
            .bInterfaceNumber = ITF_NUM_VIDEO_CONTROL, .bAlternateSetting = 0, .bNumEndpoints = 0,
            .bInterfaceClass = TUSB_CLASS_VIDEO, .bInterfaceSubClass = VIDEO_SUBCLASS_CONTROL,
            .bInterfaceProtocol = VIDEO_ITF_PROTOCOL_15, .iInterface = STRID_UVC_CONTROL,
        },
        .header = {
            .bLength = sizeof(tusb_desc_video_control_header_1itf_t), .bDescriptorType = TUSB_DESC_CS_INTERFACE,
            .bDescriptorSubType = VIDEO_CS_ITF_VC_HEADER, .bcdUVC = VIDEO_BCD_1_50,
            .wTotalLength = sizeof(uvc_control_desc_t) - sizeof(tusb_desc_interface_t),
            .dwClockFrequency = UVC_CLOCK_FREQUENCY, .bInCollection = 1,
            .baInterfaceNr = { ITF_NUM_VIDEO_STREAMING },
        },
        .camera_terminal = {
            .bLength = sizeof(tusb_desc_video_control_camera_terminal_t), .bDescriptorType = TUSB_DESC_CS_INTERFACE,
            .bDescriptorSubType = VIDEO_CS_ITF_VC_INPUT_TERMINAL, .bTerminalID = UVC_ENTITY_CAP_INPUT_TERMINAL,
            .wTerminalType = VIDEO_ITT_CAMERA, .bAssocTerminal = 0, .iTerminal = 0,
            .wObjectiveFocalLengthMin = 0, .wObjectiveFocalLengthMax = 0, .wOcularFocalLength = 0,
            .bControlSize = 3, .bmControls = { 0, 0, 0 },
        },
        .output_terminal = {
            .bLength = sizeof(tusb_desc_video_control_output_terminal_t), .bDescriptorType = TUSB_DESC_CS_INTERFACE,
            .bDescriptorSubType = VIDEO_CS_ITF_VC_OUTPUT_TERMINAL, .bTerminalID = UVC_ENTITY_CAP_OUTPUT_TERMINAL,
            .wTerminalType = VIDEO_TT_STREAMING, .bAssocTerminal = 0,
            .bSourceID = UVC_ENTITY_CAP_INPUT_TERMINAL, .iTerminal = 0,
        },
    },
    .vs = {
        .itf = {
            .bLength = sizeof(tusb_desc_interface_t), .bDescriptorType = TUSB_DESC_INTERFACE,
            .bInterfaceNumber = ITF_NUM_VIDEO_STREAMING, .bAlternateSetting = 0, .bNumEndpoints = 1, // bulk
            .bInterfaceClass = TUSB_CLASS_VIDEO, .bInterfaceSubClass = VIDEO_SUBCLASS_STREAMING,
            .bInterfaceProtocol = VIDEO_ITF_PROTOCOL_15, .iInterface = STRID_UVC_STREAMING,
        },
        .header = {
            .bLength = sizeof(tusb_desc_video_streaming_input_header_1byte_t), .bDescriptorType = TUSB_DESC_CS_INTERFACE,
            .bDescriptorSubType = VIDEO_CS_ITF_VS_INPUT_HEADER, .bNumFormats = 1,
            .wTotalLength = sizeof(uvc_streaming_desc_t) - sizeof(tusb_desc_interface_t) - sizeof(tusb_desc_endpoint_t),
            .bEndpointAddress = EPNUM_VIDEO_IN, .bmInfo = 0, .bTerminalLink = UVC_ENTITY_CAP_OUTPUT_TERMINAL,
            .bStillCaptureMethod = 0, .bTriggerSupport = 0, .bTriggerUsage = 0, .bControlSize = 1, .bmaControls = { 0 },
        },
        .format = {
            .bLength = sizeof(tusb_desc_video_format_mjpeg_t), .bDescriptorType = TUSB_DESC_CS_INTERFACE,
            .bDescriptorSubType = VIDEO_CS_ITF_VS_FORMAT_MJPEG, .bFormatIndex = 1, .bNumFrameDescriptors = 1,
            .bmFlags = 0, .bDefaultFrameIndex = 1, .bAspectRatioX = 0, .bAspectRatioY = 0,
            .bmInterlaceFlags = 0, .bCopyProtect = 0,
        },
        .frame = {
            .bLength = sizeof(tusb_desc_video_frame_mjpeg_continuous_t), .bDescriptorType = TUSB_DESC_CS_INTERFACE,
            .bDescriptorSubType = VIDEO_CS_ITF_VS_FRAME_MJPEG, .bFrameIndex = 1, .bmCapabilities = 0,
            .wWidth = FALCON_FRAME_W, .wHeight = FALCON_FRAME_H,
            .dwMinBitRate = FALCON_FRAME_W * FALCON_FRAME_H * 16 * 1,
            .dwMaxBitRate = FALCON_FRAME_W * FALCON_FRAME_H * 16 * FALCON_FRAME_RATE,
            .dwMaxVideoFrameBufferSize = FALCON_FRAME_W * FALCON_FRAME_H * 2,
            .dwDefaultFrameInterval = 10000000 / FALCON_FRAME_RATE, .bFrameIntervalType = 0,
            .dwFrameInterval = { 10000000 / FALCON_FRAME_RATE, 10000000, 10000000 / FALCON_FRAME_RATE },
        },
        .color = {
            .bLength = sizeof(tusb_desc_video_streaming_color_matching_t), .bDescriptorType = TUSB_DESC_CS_INTERFACE,
            .bDescriptorSubType = VIDEO_CS_ITF_VS_COLORFORMAT, .bColorPrimaries = VIDEO_COLOR_PRIMARIES_BT709,
            .bTransferCharacteristics = VIDEO_COLOR_XFER_CH_BT709, .bMatrixCoefficients = VIDEO_COLOR_COEF_SMPTE170M,
        },
        .ep = {
            .bLength = sizeof(tusb_desc_endpoint_t), .bDescriptorType = TUSB_DESC_ENDPOINT,
            .bEndpointAddress = EPNUM_VIDEO_IN,
            .bmAttributes = { .xfer = TUSB_XFER_BULK, .sync = 0 },
            .wMaxPacketSize = 64, .bInterval = 1,
        },
    },
};

const uint8_t *tud_descriptor_configuration_cb(uint8_t index) {
    (void)index;
    return (const uint8_t *)&s_desc_cfg;
}

// ---- Strings -------------------------------------------------------------
static const char *const s_strings[] = {
    (const char[]){ 0x09, 0x04 },
    "Team Resurgent",
    "Falcon Camera (dev UVC)",
    "Falcon Control",
    "Falcon Streaming",
};

const uint16_t *tud_descriptor_string_cb(uint8_t index, uint16_t langid) {
    (void)langid;
    static uint16_t buf[32];
    uint8_t len;
    if (index == 0) { buf[1] = 0x0409; len = 1; }
    else {
        if (index >= sizeof(s_strings) / sizeof(s_strings[0])) return NULL;
        const char *s = s_strings[index];
        len = (uint8_t)strlen(s);
        if (len > 31) len = 31;
        for (uint8_t i = 0; i < len; i++) buf[1 + i] = s[i];
    }
    buf[0] = (uint16_t)((TUSB_DESC_STRING << 8) | (2 * len + 2));
    return buf;
}
