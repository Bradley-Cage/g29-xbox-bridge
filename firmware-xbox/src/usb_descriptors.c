/**
 * usb_descriptors.c — Xbox GIP USB Descriptors (Placeholder)
 *
 * TODO: Phase 2 — replace with proper GIP wheel descriptors
 * These are minimal placeholders to satisfy the TinyUSB linker.
 */

#include "tusb.h"
#include "gip_protocol.h"
#include <string.h>

// ─── Device Descriptor ───

static const tusb_desc_device_t device_desc = {
    .bLength            = sizeof(tusb_desc_device_t),
    .bDescriptorType    = TUSB_DESC_DEVICE,
    .bcdUSB             = 0x0200,
    .bDeviceClass       = 0xFF,         // Vendor Specific
    .bDeviceSubClass    = 0x47,         // GIP
    .bDeviceProtocol    = 0xD0,         // GIP
    .bMaxPacketSize0    = 64,
    .idVendor           = XBOX_VID,
    .idProduct          = XBOX_PID,
    .bcdDevice          = 0x0507,
    .iManufacturer      = 1,
    .iProduct           = 2,
    .iSerialNumber      = 3,
    .bNumConfigurations = 1
};

const uint8_t *tud_descriptor_device_cb(void) {
    return (const uint8_t *)&device_desc;
}

// ─── Configuration Descriptor ───

#define CONFIG_TOTAL_LEN (TUD_CONFIG_DESC_LEN + 9 + 7 + 7)

static const uint8_t config_desc[] = {
    // Config
    TUD_CONFIG_DESC_LEN, TUSB_DESC_CONFIGURATION,
    U16_TO_U8S_LE(CONFIG_TOTAL_LEN), 1, 1, 0, 0x80, 250,

    // Interface: Vendor (GIP)
    9, TUSB_DESC_INTERFACE, 0, 0, 2, 0xFF, 0x47, 0xD0, 0,

    // EP IN (controller → Xbox)
    7, TUSB_DESC_ENDPOINT, 0x81, TUSB_XFER_INTERRUPT, U16_TO_U8S_LE(64), 1,

    // EP OUT (Xbox → controller)
    7, TUSB_DESC_ENDPOINT, 0x01, TUSB_XFER_INTERRUPT, U16_TO_U8S_LE(64), 1,
};

const uint8_t *tud_descriptor_configuration_cb(uint8_t index) {
    (void)index;
    return config_desc;
}

// ─── String Descriptors ───

static const char *string_descs[] = {
    [0] = (const char[]){0x09, 0x04},   // English US
    [1] = "Microsoft",
    [2] = "Xbox Wireless Controller",
    [3] = "000000000001",
};

const uint16_t *tud_descriptor_string_cb(uint8_t index, uint16_t langid) {
    (void)langid;

    static uint16_t desc_str[64];

    if (index == 0) {
        desc_str[0] = (TUSB_DESC_STRING << 8) | 4;
        desc_str[1] = 0x0409;
        return desc_str;
    }

    if (index >= sizeof(string_descs) / sizeof(string_descs[0])) return NULL;

    const char *str = string_descs[index];
    uint8_t len = (uint8_t)strlen(str);
    if (len > 62) len = 62;

    desc_str[0] = (TUSB_DESC_STRING << 8) | (2 + 2 * len);
    for (uint8_t i = 0; i < len; i++) {
        desc_str[i + 1] = str[i];
    }

    return desc_str;
}
