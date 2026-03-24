/**
 * tusb_config.h — TinyUSB Configuration for RP2040 #2 (G29 + Auth)
 *
 * This chip runs USB HOST only (PIO-USB) — no device stack.
 * G29 and PowerA auth board connect via PIO USB host.
 */

#ifndef _TUSB_CONFIG_H_
#define _TUSB_CONFIG_H_

#ifdef __cplusplus
extern "C" {
#endif

// ─── Common ───
#define CFG_TUSB_OS             OPT_OS_PICO
#define CFG_TUSB_DEBUG          1

#ifndef CFG_TUSB_MEM_SECTION
#define CFG_TUSB_MEM_SECTION
#endif

#ifndef CFG_TUSB_MEM_ALIGN
#define CFG_TUSB_MEM_ALIGN      __attribute__((aligned(4)))
#endif

// ─── Device: DISABLED ───
#define CFG_TUD_ENABLED         0

// ─── Host: ENABLED via PIO-USB ───
#define CFG_TUH_ENABLED         1
#define CFG_TUH_RPI_PIO_USB     1
#define CFG_TUH_MAX_SPEED       OPT_MODE_FULL_SPEED

// Enumeration buffer
#define CFG_TUH_ENUMERATION_BUFSIZE  256

// Hub support (for potential future use; currently using dual root ports)
#define CFG_TUH_HUB             0

// Max devices: G29 + auth board = 2 (but allow headroom)
#define CFG_TUH_DEVICE_MAX      4

// HID host class — for G29 report reception
#define CFG_TUH_HID             4
#define CFG_TUH_HID_EPIN_BUFSIZE    64
#define CFG_TUH_HID_EPOUT_BUFSIZE   64

// Vendor class — disabled for now (pipe_handle_t compatibility issue)
// Auth board communication will use raw control transfers instead
#define CFG_TUH_VENDOR          0

#ifdef __cplusplus
}
#endif

#endif // _TUSB_CONFIG_H_
