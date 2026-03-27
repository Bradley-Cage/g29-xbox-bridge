/**
 * g29_host.c — G29 PIO USB Host via TinyUSB Host Stack
 *
 * Uses TinyUSB tuh_* APIs with Pico-PIO-USB backend.
 * Handles: G29 enumeration, mode switch, HID report polling, FFB output.
 */

#include "g29_host.h"
#include "g29_parser.h"
// auth_relay removed — auth now handled on Board X
#include "bridge_config.h"

#include "pio_usb.h"
#include "pio_usb_ll.h"
#include "usb_definitions.h"
#include "tusb.h"
#include "hardware/gpio.h"
#include "hardware/sync.h"

#include <stdio.h>
#include <string.h>

// ─── State ───

static uint8_t g29_dev_addr = 0;
static uint8_t g29_hid_instance = 0;
static uint8_t status = G29_STATUS_DISCONNECTED;
static bool initialized = false;
static g29_state_t current_state;
static volatile bool new_report_available = false;

// Deferred mode switch — can't send USB transfers inside tuh_mount_cb
static volatile bool mode_switch_pending = false;
static uint8_t mode_switch_addr = 0;
static uint8_t mode_switch_attempts = 0;
#define MODE_SWITCH_MAX_ATTEMPTS 2

// ─── Init ───

// Track spurious unmount retries — no limit, keep trying forever
static volatile uint8_t unmount_retry_count = 0;

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

    // Handle deferred mode switch (can't send USB from inside callbacks)
    // State machine: 0=idle, 1=send switch cmd, 2=wait+send revert cmd
    static uint8_t mode_switch_step = 0;
    static uint32_t mode_switch_time = 0;

    if (mode_switch_pending && mode_switch_addr != 0) {
        mode_switch_pending = false;
        mode_switch_step = 1;
        mode_switch_time = to_ms_since_boot(get_absolute_time());
    }

    if (mode_switch_step == 1) {
        // Step 1: Send switch-to-G29 command
        uint8_t cmd_switch[] = G29_CMD_MODE_G29;
        bool ok = tuh_hid_set_report(mode_switch_addr, 0, 0,
                                      HID_REPORT_TYPE_OUTPUT,
                                      cmd_switch, sizeof(cmd_switch));
        printf("[G29] Mode switch cmd1 (0x09 switch): %s\n", ok ? "sent" : "FAILED");
        mode_switch_step = 2;
        mode_switch_time = to_ms_since_boot(get_absolute_time());
    } else if (mode_switch_step == 2) {
        // Step 2: Send revert-config command after short delay
        uint32_t now = to_ms_since_boot(get_absolute_time());
        if (now - mode_switch_time >= 50) {
            uint8_t cmd_revert[] = G29_CMD_REVERT;
            bool ok = tuh_hid_set_report(mode_switch_addr, 0, 0,
                                          HID_REPORT_TYPE_OUTPUT,
                                          cmd_revert, sizeof(cmd_revert));
            printf("[G29] Mode switch cmd2 (0x0A revert cfg): %s\n", ok ? "sent" : "FAILED");
            mode_switch_step = 0;
        }
    }

    // Timeout: if mode switch hasn't caused re-enumeration in 5s, accept legacy
    if (status == G29_STATUS_MODE_SWITCHING && g29_dev_addr != 0) {
        static uint32_t mode_switch_start = 0;
        if (mode_switch_start == 0) {
            mode_switch_start = to_ms_since_boot(get_absolute_time());
        }
        uint32_t now = to_ms_since_boot(get_absolute_time());
        if (now - mode_switch_start > 5000) {
            printf("[G29] Mode switch timeout — accepting legacy mode\n");
            status = G29_STATUS_CONNECTED;
            mode_switch_start = 0;
        }
    }

    // Return (and clear) new-report flag set by tuh_hid_report_received_cb
    bool got_report = new_report_available;
    new_report_available = false;
    return got_report;
}

bool g29_send_ffb(const uint8_t cmd[7]) {
    if (g29_dev_addr == 0) return false;
    // Use HID output report to send FFB command
    return tuh_hid_set_report(g29_dev_addr, g29_hid_instance,
                              0, HID_REPORT_TYPE_OUTPUT,
                              (void *)cmd, G29_FFB_CMD_SIZE);
}

void g29_snapshot_state(g29_state_t *out) {
    // Copy under brief interrupt disable to prevent torn reads.
    // The HID callback fires from tuh_task() context (not ISR on PIO-USB),
    // but we disable interrupts to be safe against any preemption.
    uint32_t save = save_and_disable_interrupts();
    *out = current_state;
    restore_interrupts(save);
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
            g29_dev_addr = dev_addr;

            if (mode_switch_attempts < MODE_SWITCH_MAX_ATTEMPTS) {
                status = G29_STATUS_MODE_SWITCHING;
                mode_switch_addr = dev_addr;
                mode_switch_pending = true;
                mode_switch_attempts++;
                printf("[G29] Legacy (PID=%04X), mode switch attempt %d/%d\n",
                       pid, mode_switch_attempts, MODE_SWITCH_MAX_ATTEMPTS);
            } else {
                // Accept legacy mode — 8-bit steering, no clutch
                status = G29_STATUS_CONNECTED;
                printf("[G29] Mode switch failed — using legacy mode (PID=%04X)\n", pid);
            }
        }
    } else {
        // Not a G29 — ignore (auth relay moved to Board X)
        printf("[USB] Non-G29 device ignored: VID=%04X PID=%04X\n", vid, pid);
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
        unmount_retry_count = 0;  // Real device — reset retry counter
    } else if (g29_dev_addr == 0) {
        // Spurious unmount: PIO-USB detected the device and started
        // enumeration, but it failed (common with already-connected devices).
        // Force connected=false so PIO-USB re-detects the pull-up and retries.
        unmount_retry_count++;
        if (unmount_retry_count <= 5 || (unmount_retry_count % 10) == 0) {
            printf("[G29] Spurious unmount — forcing re-detect (retry %d)\n",
                   unmount_retry_count);
        }
        root_port_t *root = PIO_USB_ROOT_PORT(0);
        root->connected = false;
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
        // Dump reports in hex — only when data changes, skip empty
        static uint8_t prev_report[32] = {0};
        static bool first_report = true;
        if (len > 0 && (first_report || memcmp(report, prev_report, len) != 0)) {
            printf("[RAW] len=%d: ", len);
            for (uint16_t i = 0; i < len; i++) printf("%02X ", report[i]);
            printf("\n");
            memcpy(prev_report, report, len < 32 ? len : 32);
            first_report = false;
        }

        g29_parse_report(report, len, &current_state);
        new_report_available = true;

        // Re-request next report
        tuh_hid_receive_report(dev_addr, instance);
    }
}
