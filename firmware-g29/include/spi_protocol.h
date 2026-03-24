/**
 * spi_protocol.h — SPI Frame Definitions
 *
 * Shared between firmware-xbox (RP2040 #1, SPI master)
 * and firmware-g29 (RP2040 #2, SPI slave).
 *
 * Fixed 32-byte frames exchanged at 1kHz.
 * CRC-8 on every frame. All values little-endian.
 */

#ifndef SPI_PROTOCOL_H
#define SPI_PROTOCOL_H

#include <stdint.h>
#include <stdbool.h>

// Frame size — fixed, never changes
#define SPI_FRAME_SIZE      32
#define SPI_CRC_OFFSET      31

// SPI bus parameters
#define SPI_CLOCK_HZ        10000000    // 10 MHz
#define SPI_FRAME_RATE_HZ   1000        // 1 kHz
#define SPI_TIMEOUT_MS      50          // Error state if no valid frame in 50ms
#define SPI_AUTH_SEQ_TIMEOUT_MS  500    // Multi-frame auth sequence timeout

// ─── Frame Types ───

// Upstream: RP2040 #2 → #1
#define FRAME_UP_AXIS_STATE     0x01    // G29 axis + button state
#define FRAME_UP_AUTH_RESPONSE  0x02    // Auth board response data
#define FRAME_UP_STATUS         0x03    // Device status report

// Downstream: RP2040 #1 → #2
#define FRAME_DN_FORCE_OUTPUT   0x01    // Af force values for G29
#define FRAME_DN_AUTH_CHALLENGE 0x02    // Auth challenge from Xbox
#define FRAME_DN_CONFIG         0x03    // Configuration commands

// ─── Command Flags (downstream byte 17) ───

#define CMD_FLAG_STOP_ALL_FFB   (1 << 0)
#define CMD_FLAG_SET_RANGE      (1 << 1)
#define CMD_FLAG_SET_POWER      (1 << 2)

// ─── Connection Status (upstream byte 18) ───

#define G29_STATUS_DISCONNECTED     0
#define G29_STATUS_CONNECTED        1
#define G29_STATUS_MODE_SWITCHING   2

#define AUTH_STATUS_DISCONNECTED    0
#define AUTH_STATUS_CONNECTED       1

// ─── Upstream Frame: G29 State (RP2040 #2 → #1) ───

typedef struct __attribute__((packed)) {
    uint8_t  frame_type;        // 0: FRAME_UP_*
    uint8_t  sequence;          // 1: wrapping 0-255
    uint16_t steering;          // 2-3: raw 16-bit LE from G29
    uint8_t  gas;               // 4: 0xFF=released, 0x00=pressed
    uint8_t  brake;             // 5
    uint8_t  clutch;            // 6
    uint8_t  buttons0;          // 7: dpad(3:0) + face(7:4)
    uint8_t  buttons1;          // 8: R1,L1,R2,L2,Share,Options,R3,L3
    uint8_t  buttons2;          // 9: shifter gears + plus
    uint8_t  buttons3;          // 10: minus, dial, PS
    float    steering_velocity; // 11-14: Av (computed by RP2040 #2)
    float    steering_accel;    // 15-18: Aa (computed by RP2040 #2)
    uint8_t  g29_status;        // 19: G29_STATUS_*
    uint8_t  auth_status;       // 20: AUTH_STATUS_*
    uint8_t  error_count;       // 21: wrapping error counter
    uint8_t  reserved[9];       // 22-30: reserved / auth response data
    uint8_t  crc;               // 31: CRC-8
} spi_upstream_frame_t;

_Static_assert(sizeof(spi_upstream_frame_t) == SPI_FRAME_SIZE,
               "Upstream frame must be exactly 32 bytes");

// ─── Downstream Frame: Force + Commands (RP2040 #1 → #2) ───

typedef struct __attribute__((packed)) {
    uint8_t  frame_type;        // 0: FRAME_DN_*
    uint8_t  sequence;          // 1: wrapping 0-255
    float    af_wheel;          // 2-5: Af axis 0, wheel torque (-1.0 to 1.0)
    float    af_motor_low;      // 6-9: Af axis 5, low freq vibration
    float    af_motor_high;     // 10-13: Af axis 6, high freq vibration
    uint8_t  power_level;       // 14: 0-255
    uint16_t angle_setting;     // 15-16: degrees (LE)
    uint8_t  cmd_flags;         // 17: CMD_FLAG_*
    uint8_t  reserved[13];      // 18-30: reserved / auth challenge data
    uint8_t  crc;               // 31: CRC-8
} spi_downstream_frame_t;

_Static_assert(sizeof(spi_downstream_frame_t) == SPI_FRAME_SIZE,
               "Downstream frame must be exactly 32 bytes");

// ─── CRC-8 (polynomial 0x07, init 0x00) ───

static inline uint8_t spi_crc8(const uint8_t *data, uint8_t len) {
    uint8_t crc = 0x00;
    for (uint8_t i = 0; i < len; i++) {
        crc ^= data[i];
        for (uint8_t bit = 0; bit < 8; bit++) {
            if (crc & 0x80) {
                crc = (crc << 1) ^ 0x07;
            } else {
                crc <<= 1;
            }
        }
    }
    return crc;
}

// ─── Frame Validation ───

static inline bool spi_validate_frame(const uint8_t *frame) {
    uint8_t computed = spi_crc8(frame, SPI_FRAME_SIZE - 1);
    return computed == frame[SPI_CRC_OFFSET];
}

static inline void spi_set_crc(uint8_t *frame) {
    frame[SPI_CRC_OFFSET] = spi_crc8(frame, SPI_FRAME_SIZE - 1);
}

#endif // SPI_PROTOCOL_H
