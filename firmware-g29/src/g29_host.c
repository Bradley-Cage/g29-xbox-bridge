/**
 * g29_host.c — G29 PIO USB Host via TinyUSB Host Stack
 *
 * Uses TinyUSB tuh_* APIs with Pico-PIO-USB backend.
 * Handles: G29 enumeration, mode switch, HID report polling, FFB output.
 */

#include "g29_host.h"
#include "g29_parser.h"
#include "bridge_config.h"

#include "pio_usb.h"
#include "tusb.h"

#include <stdio.h>
#include <string.h>

// ─── State ───

static uint8_t g29_dev_addr = 0;
static uint8_t g29_hid_instance = 0;
static uint8_t status = G29_STATUS_DISCONNECTED;
static bool initialized = false;
static g29_state_t current_state;

// ─── Init ───

bool g29_host_init(void) {
    // Configure PIO-USB for TinyUSB host
    pio_usb_configuration_t pio_cfg = PIO_USB_DEFAULT_CONFIG;
    pio_cfg.pin_dp = G29_USB_DP_PIN;
    pio_cfg.pinout = PIO_USB_PINOUT_DPDM;

    tuh_configure(1, TUH_CFGID_RPI_PIO_USB_CONFIGURATION, &pio_cfg);
    tuh_init(1);

    initialized = true;
    printf("[G29] TinyUSB host initialized on PIO, GPIO %d/%d\n",
           G29_USB_DP_PIN, G29_USB_DM_PIN);
    return true;
}

bool g29_host_task(void) {
    if (!initialized) return false;
    tuh_task();
    return false; // TODO: return true when new report received
}

bool g29_send_ffb(const uint8_t cmd[7]) {
    if (g29_dev_addr == 0) return false;
    // Use HID output report to send FFB command
    return tuh_hid_set_report(g29_dev_addr, g29_hid_instance,
                              0, HID_REPORT_TYPE_OUTPUT,
                              (void *)cmd, G29_FFB_CMD_SIZE);
}

bool g29_is_connected(void) {
    return status == G29_STATUS_CONNECTED;
}

uint8_t g29_get_status(void) {
    return status;
}

// ─── TinyUSB Host Callbacks ───

// Called when a device is mounted (enumerated)
void tuh_mount_cb(uint8_t dev_addr) {
    uint16_t vid, pid;
    tuh_vid_pid_get(dev_addr, &vid, &pid);
    printf("[USB] Device mounted: addr=%d VID=%04X PID=%04X\n", dev_addr, vid, pid);

    if (vid == G29_VID) {
        if (pid == G29_PID_PS3) {
            // Already in G29 native mode
            g29_dev_addr = dev_addr;
            status = G29_STATUS_CONNECTED;
            printf("[G29] Native G29 detected (PID=%04X)\n", pid);
        } else if (pid == G29_PID_LEGACY) {
            // Legacy mode — need mode switch
            status = G29_STATUS_MODE_SWITCHING;
            g29_dev_addr = dev_addr;
            printf("[G29] Legacy Logitech detected (PID=%04X), sending mode switch\n", pid);
            // TODO: Send mode switch commands via control transfer
        }
    }
}

// Called when a device is unmounted
void tuh_umount_cb(uint8_t dev_addr) {
    printf("[USB] Device unmounted: addr=%d\n", dev_addr);
    if (dev_addr == g29_dev_addr) {
        if (status == G29_STATUS_MODE_SWITCHING) {
            printf("[G29] Disconnected during mode switch — waiting for re-enumeration\n");
        } else {
            printf("[G29] Disconnected\n");
            status = G29_STATUS_DISCONNECTED;
        }
        g29_dev_addr = 0;
    }
}

// Called when HID interface is mounted
void tuh_hid_mount_cb(uint8_t dev_addr, uint8_t instance,
                       uint8_t const *desc_report, uint16_t desc_len) {
    (void)desc_report;
    (void)desc_len;

    uint16_t vid, pid;
    tuh_vid_pid_get(dev_addr, &vid, &pid);

    printf("[HID] Mounted: addr=%d inst=%d VID=%04X PID=%04X\n",
           dev_addr, instance, vid, pid);

    if (vid == G29_VID && (pid == G29_PID_PS3 || pid == G29_PID_LEGACY)) {
        g29_hid_instance = instance;
        // Start receiving HID reports
        if (!tuh_hid_receive_report(dev_addr, instance)) {
            printf("[HID] Failed to request report\n");
        } else {
            printf("[HID] Report reception started\n");
        }
    }
}

// Called when HID interface is unmounted
void tuh_hid_umount_cb(uint8_t dev_addr, uint8_t instance) {
    printf("[HID] Unmounted: addr=%d inst=%d\n", dev_addr, instance);
}

// Called when HID report is received
void tuh_hid_report_received_cb(uint8_t dev_addr, uint8_t instance,
                                 uint8_t const *report, uint16_t len) {
    if (dev_addr == g29_dev_addr) {
        g29_parse_report(report, (uint8_t)len, &current_state);

        // Re-request next report
        tuh_hid_receive_report(dev_addr, instance);
    }
}
