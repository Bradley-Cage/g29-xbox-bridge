/**
 * usb_descriptors.c — Xbox GIP USB Descriptors
 *
 * Based on MS-GIPUSB specification and GP2040-CE proven implementation.
 * Key requirements:
 *   - Device class/subclass/protocol: 0xFF/0x47/0xD0 (GIP)
 *   - MS OS String Descriptor at index 0xEE with "XGIP10" compatible ID
 *   - STALL on device_qualifier requests (full-speed only)
 *   - Interrupt endpoints IN 0x81 + OUT 0x02, 64 bytes, 4ms interval
 */

#include "tusb.h"
#include "gip_protocol.h"
#include "pico/unique_id.h"
#include <string.h>
#include <stdio.h>

// ─── VID/PID ───
// GP2040-CE uses PDP VID 0x0E6F / PID 0x02A4 — proven on Xbox Series X
// with auth passthrough. Using Microsoft VID 0x045E may cause issues
// without a matching auth certificate.
#define GIP_VID  0x0E6F  // PDP (Performance Designed Products)
#define GIP_PID  0x02A4  // GP2040-CE compatible

// ─── MS OS Descriptor Constants ───
#define MS_OS_VENDOR_CODE  0x90

// ─── Device Descriptor ───

static const tusb_desc_device_t device_desc = {
    .bLength            = sizeof(tusb_desc_device_t),
    .bDescriptorType    = TUSB_DESC_DEVICE,
    .bcdUSB             = 0x0200,       // USB 2.0
    .bDeviceClass       = 0xFF,         // Vendor Specific
    .bDeviceSubClass    = 0x47,         // GIP
    .bDeviceProtocol    = 0xD0,         // GIP
    .bMaxPacketSize0    = 64,
    .idVendor           = GIP_VID,
    .idProduct          = GIP_PID,
    .bcdDevice          = 0x0507,       // Firmware version 5.7
    .iManufacturer      = 1,
    .iProduct           = 2,
    .iSerialNumber      = 3,
    .bNumConfigurations = 1
};

const uint8_t *tud_descriptor_device_cb(void) {
    return (const uint8_t *)&device_desc;
}

// ─── Configuration Descriptor ───
// 9 (config) + 9 (interface) + 7 (EP IN) + 7 (EP OUT) = 32 bytes

static const uint8_t config_desc[] = {
    // Configuration descriptor (9 bytes)
    0x09, TUSB_DESC_CONFIGURATION,
    U16_TO_U8S_LE(32),             // wTotalLength = 32
    0x01,                           // bNumInterfaces = 1 (no audio)
    0x01,                           // bConfigurationValue = 1
    0x00,                           // iConfiguration = 0
    0x80,                           // bmAttributes: bus-powered (no remote wakeup — not implemented)
    0xFA,                           // bMaxPower = 500mA

    // Interface 0: GIP Data (9 bytes)
    0x09, TUSB_DESC_INTERFACE,
    0x00,                           // bInterfaceNumber = 0
    0x00,                           // bAlternateSetting = 0
    0x02,                           // bNumEndpoints = 2
    0xFF,                           // bInterfaceClass = vendor-specific
    0x47,                           // bInterfaceSubClass = GIP
    0xD0,                           // bInterfaceProtocol = GIP
    0x00,                           // iInterface = 0

    // Endpoint IN: controller → Xbox (7 bytes)
    0x07, TUSB_DESC_ENDPOINT,
    0x81,                           // bEndpointAddress = IN 1
    TUSB_XFER_INTERRUPT,            // bmAttributes = interrupt
    U16_TO_U8S_LE(64),             // wMaxPacketSize = 64
    0x04,                           // bInterval = 4ms (250Hz)

    // Endpoint OUT: Xbox → controller (7 bytes)
    0x07, TUSB_DESC_ENDPOINT,
    0x02,                           // bEndpointAddress = OUT 2
    TUSB_XFER_INTERRUPT,            // bmAttributes = interrupt
    U16_TO_U8S_LE(64),             // wMaxPacketSize = 64
    0x04,                           // bInterval = 4ms (250Hz)
};

const uint8_t *tud_descriptor_configuration_cb(uint8_t index) {
    (void)index;
    return config_desc;
}

// ─── MS OS 2.0 Compatible ID Feature Descriptor ───
// "XGIP10" — triggers Xbox GIP driver binding
// Returned in response to vendor request with MS_OS_VENDOR_CODE

static const uint8_t ms_os_compat_id_desc[] = {
    // Header (16 bytes)
    0x28, 0x00, 0x00, 0x00,         // dwLength = 40
    0x00, 0x01,                     // bcdVersion = 1.0
    0x04, 0x00,                     // wIndex = 4 (Extended CompatID)
    0x01,                           // bCount = 1 function
    0x00, 0x00, 0x00, 0x00,         // Reserved (7 bytes)
    0x00, 0x00, 0x00,

    // Function section (24 bytes)
    0x00,                           // bFirstInterfaceNumber = 0
    0x01,                           // Reserved (must be 0x01 per MS OS 1.0 spec)
    'X', 'G', 'I', 'P', '1', '0', 0x00, 0x00,  // compatibleID = "XGIP10"
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,  // subCompatibleID (empty)
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00,               // Reserved (6 bytes)
};

// ─── String Descriptors ───

// Serial number: 32 hex chars from Pico unique board ID
static char serial_str[33];  // "000102030405060708090A0B0C0D0E0F"
static bool serial_init = false;

static void init_serial(void) {
    if (serial_init) return;
    pico_unique_board_id_t id;
    pico_get_unique_board_id(&id);
    for (int i = 0; i < 8; i++) {
        sprintf(&serial_str[i * 2], "%02X", id.id[i]);
    }
    // Pad to 32 chars (Xbox expects 32 hex digits)
    memset(&serial_str[16], '0', 16);
    serial_str[32] = '\0';
    serial_init = true;
}

// MS OS String Descriptor at index 0xEE
static const uint8_t ms_os_string_desc[] = {
    0x12,                           // bLength = 18
    TUSB_DESC_STRING,               // bDescriptorType = STRING
    // "MSFT100" in UTF-16LE
    'M', 0, 'S', 0, 'F', 0, 'T', 0, '1', 0, '0', 0, '0', 0,
    MS_OS_VENDOR_CODE,              // bMS_VendorCode
    0x00                            // bPad
};

static const char *string_descs[] = {
    [0] = (const char[]){0x09, 0x04},   // English US LANGID
    [1] = "Open Stick Community",
    [2] = "G29 Xbox Bridge",
    [3] = NULL,  // Serial — generated at runtime
};

const uint16_t *tud_descriptor_string_cb(uint8_t index, uint16_t langid) {
    (void)langid;

    static uint16_t desc_str[64];

    // MS OS String Descriptor at index 0xEE
    if (index == 0xEE) {
        memcpy(desc_str, ms_os_string_desc, sizeof(ms_os_string_desc));
        return desc_str;
    }

    // LANGID
    if (index == 0) {
        desc_str[0] = (TUSB_DESC_STRING << 8) | 4;
        desc_str[1] = 0x0409;
        return desc_str;
    }

    // Serial number — dynamic from board unique ID
    if (index == 3) {
        init_serial();
        uint8_t len = 32;
        desc_str[0] = (TUSB_DESC_STRING << 8) | (2 + 2 * len);
        for (uint8_t i = 0; i < len; i++) {
            desc_str[i + 1] = serial_str[i];
        }
        return desc_str;
    }

    if (index >= sizeof(string_descs) / sizeof(string_descs[0])) return NULL;

    const char *str = string_descs[index];
    if (!str) return NULL;

    uint8_t len = (uint8_t)strlen(str);
    if (len > 62) len = 62;

    desc_str[0] = (TUSB_DESC_STRING << 8) | (2 + 2 * len);
    for (uint8_t i = 0; i < len; i++) {
        desc_str[i + 1] = str[i];
    }

    return desc_str;
}

// ─── Vendor Control Request Handler ───
// Handles MS OS Descriptor requests (vendor code 0x90)
// and STALL on device_qualifier (full-speed only device)

bool tud_vendor_control_xfer_cb(uint8_t rhport, uint8_t stage,
                                 tusb_control_request_t const *request) {
    (void)rhport;

    // MS OS Compatible ID request
    if (request->bmRequestType_bit.type == TUSB_REQ_TYPE_VENDOR &&
        request->bRequest == MS_OS_VENDOR_CODE &&
        request->wIndex == 0x0004) {

        if (stage == CONTROL_STAGE_SETUP) {
            uint16_t len = sizeof(ms_os_compat_id_desc);
            if (request->wLength < len) len = request->wLength;
            tud_control_xfer(rhport, request,
                             (void *)ms_os_compat_id_desc, len);
        }
        return true;
    }

    // STALL everything else (including device_qualifier)
    return false;
}
