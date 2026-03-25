/**
 * tusb_config.h — TinyUSB Configuration for RP2040 #1 (Xbox + Auth Host)
 *
 * Dual-stack: native USB device (Xbox GIP) + PIO-USB host (PowerA auth).
 * Port 0 (native USB PHY) = device mode → Xbox Series X
 * Port 1 (PIO-USB on GPIO 0/1) = host mode → PowerA controller
 */

#ifndef _TUSB_CONFIG_H_
#define _TUSB_CONFIG_H_

#ifdef __cplusplus
extern "C" {
#endif

// ─── Common ───
#define CFG_TUSB_MCU            OPT_MCU_RP2040
#define CFG_TUSB_OS             OPT_OS_PICO
#define CFG_TUSB_DEBUG          0  // Must be 0: dual-core TUD+TUH share debug state
#define CFG_TUSB_RHPORT0_MODE   OPT_MODE_DEVICE  // Native USB = device
#define CFG_TUSB_RHPORT1_MODE   OPT_MODE_HOST    // PIO-USB = host

#ifndef CFG_TUSB_MEM_SECTION
#define CFG_TUSB_MEM_SECTION
#endif

#ifndef CFG_TUSB_MEM_ALIGN
#define CFG_TUSB_MEM_ALIGN      __attribute__((aligned(4)))
#endif

// ─── Device (Port 0): Xbox GIP ───
#define CFG_TUD_ENABLED         1
#define CFG_TUD_ENDPOINT0_SIZE  64

// Vendor class for Xbox GIP protocol
#define CFG_TUD_VENDOR          1
#define CFG_TUD_VENDOR_RX_BUFSIZE  64
#define CFG_TUD_VENDOR_TX_BUFSIZE  64

// No other device classes needed
#define CFG_TUD_HID             0
#define CFG_TUD_CDC             0
#define CFG_TUD_MSC             0
#define CFG_TUD_MIDI            0

// ─── Host (Port 1): PIO-USB → PowerA auth ───
#define CFG_TUH_ENABLED         1
#define CFG_TUH_RPI_PIO_USB     1
#define CFG_TUH_MAX_SPEED       OPT_MODE_FULL_SPEED
#define CFG_TUH_ENUMERATION_BUFSIZE  256

// We open endpoints manually — no class drivers needed
#define CFG_TUH_HID             0
#define CFG_TUH_CDC             0
#define CFG_TUH_MSC             0
#define CFG_TUH_VENDOR          0

#ifdef __cplusplus
}
#endif

#endif // _TUSB_CONFIG_H_
