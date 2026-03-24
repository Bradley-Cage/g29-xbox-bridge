/**
 * tusb_config.h — TinyUSB Configuration for RP2040 #1 (Xbox Device)
 *
 * This chip runs USB DEVICE only (native USB → Xbox Series X).
 * No host stack needed — G29/auth are on RP2040 #2.
 */

#ifndef _TUSB_CONFIG_H_
#define _TUSB_CONFIG_H_

#ifdef __cplusplus
extern "C" {
#endif

// ─── Common ───
#define CFG_TUSB_MCU            OPT_MCU_RP2040
#define CFG_TUSB_OS             OPT_OS_PICO
#define CFG_TUSB_DEBUG          1

#ifndef CFG_TUSB_MEM_SECTION
#define CFG_TUSB_MEM_SECTION
#endif

#ifndef CFG_TUSB_MEM_ALIGN
#define CFG_TUSB_MEM_ALIGN      __attribute__((aligned(4)))
#endif

// ─── Device: ENABLED ───
#define CFG_TUD_ENABLED         1
#define CFG_TUD_ENDPOINT0_SIZE  64

// Vendor class for Xbox GIP protocol
#define CFG_TUD_VENDOR          1
#define CFG_TUD_VENDOR_RX_BUFSIZE  64
#define CFG_TUD_VENDOR_TX_BUFSIZE  64

// No other classes needed
#define CFG_TUD_HID             0
#define CFG_TUD_CDC             0
#define CFG_TUD_MSC             0
#define CFG_TUD_MIDI            0

// ─── Host: DISABLED ───
#define CFG_TUH_ENABLED         0

#ifdef __cplusplus
}
#endif

#endif // _TUSB_CONFIG_H_
