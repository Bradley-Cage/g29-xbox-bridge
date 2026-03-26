/**
 * bridge_config.h — Hardware Configuration for RP2040 #2 (G29)
 */

#ifndef BRIDGE_CONFIG_H
#define BRIDGE_CONFIG_H

#include <stdint.h>
#include <stdbool.h>

// ─── Pin Assignments ───

// PIO0 USB Host: G29 Wheel (via 3×200Ω parallel ≈67Ω series resistors)
#define G29_USB_DP_PIN      0   // GPIO 0 → 3×200Ω → USB-A breakout D+
#define G29_USB_DM_PIN      1   // GPIO 1 → 3×200Ω → USB-A breakout D-

// NOTE: Auth board (PowerA) moved to Board X (RP2040 #1) GPIO 0/1.
// GPIO 2/3 on this board are now free.

// SPI1 — Communication to RP2040 #1 (slave mode)
#define SPI_PORT            spi1
#define SPI_SCK_PIN         10
#define SPI_MOSI_PIN        11  // Data from master (RP2040 #1)
#define SPI_MISO_PIN        8   // Data to master (RP2040 #1)
#define SPI_CS_PIN          9

// WS2812 RGB LED (DWEII RP2040-Zero onboard NeoPixel)
#define WS2812_LED_PIN      16  // GPIO 16 — WS2812 RGB LED

// NOTE: GPIO 16 was previously assigned to UART_TX_PIN for debug output,
// but UART stdio is disabled (we use CDC debug over native USB instead).
// The DWEII RP2040-Zero has a WS2812 LED on GPIO 16, not a UART pin.

// ─── Xbox GIP USB Interface Class ───
// All Xbox controllers present this interface for GIP communication
#define GIP_IFACE_CLASS     0xFF    // Vendor-specific
#define GIP_IFACE_SUBCLASS  0x47    // GIP
#define GIP_IFACE_PROTOCOL  0xD0    // GIP protocol

// ─── GIP Command IDs (subset needed for auth host) ───
#define GIP_HOST_CMD_ACK         0x01
#define GIP_HOST_CMD_ANNOUNCE    0x02
#define GIP_HOST_CMD_STATUS      0x03
#define GIP_HOST_CMD_IDENTIFY    0x04
#define GIP_HOST_CMD_POWER       0x05
#define GIP_HOST_CMD_AUTHENTICATE 0x06

// GIP Power States
#define GIP_HOST_STATE_ON        0x00

// GIP Header Flags
#define GIP_HOST_FLAG_ACK        0x10
#define GIP_HOST_FLAG_INTERNAL   0x20
#define GIP_HOST_FLAG_CHUNK_START 0x40
#define GIP_HOST_FLAG_CHUNKED    0x80

// ─── USB Identifiers ───

// Logitech G29
#define G29_VID             0x046D
#define G29_PID_LEGACY      0xC294  // Pre-mode-switch (Driving Force GT)
#define G29_PID_PS3         0xC24F  // Native G29 PS3 mode (after mode switch)
#define G29_PID_PS4         0xC260  // PS4 mode (DO NOT USE)

// ─── G29 HID Report ───

#define G29_REPORT_SIZE_LEGACY  27  // Legacy DFGT mode (PID 0xC294)
#define G29_REPORT_SIZE_NATIVE  12  // Native G29 PS3 mode (PID 0xC24F)
#define G29_REPORT_SIZE     G29_REPORT_SIZE_LEGACY  // Current active mode
#define G29_PEDAL_RELEASED  0xFF
#define G29_PEDAL_PRESSED   0x00
#define G29_DPAD_NONE       0x08

// ─── G29 Mode Switch Commands (7 bytes each) ───

#define G29_CMD_REVERT      {0xF8, 0x0A, 0x00, 0x00, 0x00, 0x00, 0x00}
#define G29_CMD_MODE_G29    {0xF8, 0x09, 0x05, 0x01, 0x01, 0x00, 0x00}
#define G29_CMD_STOP_ALL    {0xF3, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00}
#define G29_CMD_AUTOCENTER_OFF {0xF5, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00}

// G29 set range: F8 81 <lo> <hi> 00 00 00
// 900 degrees = 0x0384: lo=0x84, hi=0x03
#define G29_CMD_RANGE_900   {0xF8, 0x81, 0x84, 0x03, 0x00, 0x00, 0x00}

// ─── G29 FFB ───

#define G29_FFB_CMD_SIZE    7
#define G29_FFB_MIN_INTERVAL_MS  4  // Respect bInterval — max 250Hz

// FFB slot masks (upper nibble of byte 0)
#define FFB_SLOT_0          0x10
#define FFB_SLOT_1          0x20
#define FFB_SLOT_2          0x40
#define FFB_SLOT_3          0x80
#define FFB_SLOT_ALL        0xF0

// FFB command ops (lower nibble of byte 0)
#define FFB_OP_STOP         0x00
#define FFB_OP_PLAY         0x01
#define FFB_OP_PAUSE        0x03
#define FFB_OP_UPDATE       0x0C

// FFB effect types (byte 1)
#define FFB_TYPE_CONSTANT   0x00
#define FFB_TYPE_SPRING     0x0B
#define FFB_TYPE_DAMPER     0x0C
#define FFB_TYPE_FRICTION   0x0E

// ─── Timing ───

#define G29_RENUM_TIMEOUT_MS    10000   // 10s for re-enumeration after mode switch
#define G29_POLL_INTERVAL_US    1000    // 1kHz input polling
#define WATCHDOG_TIMEOUT_MS     100     // 100ms hardware watchdog

// ─── G29 Connection Status ───

#define G29_STATUS_DISCONNECTED     0
#define G29_STATUS_CONNECTED        1
#define G29_STATUS_MODE_SWITCHING   2

// ─── Diagnostics ───

#define DIAG_RING_SIZE      256     // Event ring buffer entries

#endif // BRIDGE_CONFIG_H
