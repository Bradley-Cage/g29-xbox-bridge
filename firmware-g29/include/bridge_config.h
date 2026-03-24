/**
 * bridge_config.h — Hardware Configuration for RP2040 #2 (G29 + Auth)
 */

#ifndef BRIDGE_CONFIG_H
#define BRIDGE_CONFIG_H

#include <stdint.h>
#include <stdbool.h>

// ─── Pin Assignments ───

// PIO0 USB Host Root Port 0: G29 Wheel (via 68Ω series resistors)
#define G29_USB_DP_PIN      0   // GPIO 0 → 68Ω → USB-A breakout D+
#define G29_USB_DM_PIN      1   // GPIO 1 → 68Ω → USB-A breakout D-

// PIO0 USB Host Root Port 1: Auth Board (via 68Ω series resistors)
#define AUTH_USB_DP_PIN     2   // GPIO 2 → 68Ω → Auth board D+
#define AUTH_USB_DM_PIN     3   // GPIO 3 → 68Ω → Auth board D-

// SPI1 — Communication to RP2040 #1 (slave mode)
#define SPI_PORT            spi1
#define SPI_SCK_PIN         10
#define SPI_MOSI_PIN        11  // Data from master (RP2040 #1)
#define SPI_MISO_PIN        8   // Data to master (RP2040 #1)
#define SPI_CS_PIN          9

// UART0 — Debug output
#define UART_TX_PIN         16  // GPIO 16 → USB-TTL adapter RX
#define UART_BAUD           115200

// ─── USB Identifiers ───

// Logitech G29
#define G29_VID             0x046D
#define G29_PID_LEGACY      0xC294  // Pre-mode-switch (Driving Force GT)
#define G29_PID_PS3         0xC24F  // Native G29 PS3 mode (after mode switch)
#define G29_PID_PS4         0xC260  // PS4 mode (DO NOT USE)

// ─── G29 HID Report ───

#define G29_REPORT_SIZE     12      // PS3 mode: 12 bytes
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
