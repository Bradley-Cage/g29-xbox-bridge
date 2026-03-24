/**
 * spi_protocol.h — SPI Frame Definitions
 *
 * Shared between firmware-xbox (RP2040 #1, SPI master)
 * and firmware-g29 (RP2040 #2, SPI slave).
 *
 * Fixed 32-byte frames exchanged at 1kHz.
 * CRC-8 on every frame. All values little-endian.
 *
 * IMPORTANT: No floats in wire format — Cortex-M0+ faults on misaligned
 * float access in packed structs. Use int16_t fixed-point instead.
 * Force values: int16_t, scale factor 16384 (Q1.14 fixed-point).
 *   To convert: float_val = (float)fixed_val / 16384.0f
 *               fixed_val = (int16_t)(float_val * 16384.0f)
 *   Range: -2.0 to +1.99994 (covers the -1.0 to 1.0 Af range with headroom)
 * Velocity/accel: int16_t, scale factor 256 (Q7.8 fixed-point).
 *   Range: -128.0 to +127.996
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

// Fixed-point scale factors
#define SPI_FORCE_SCALE     16384       // Q1.14 for Af values
#define SPI_VELOCITY_SCALE  256         // Q7.8 for velocity/accel

// ─── Frame Types ───

// Upstream: RP2040 #2 → #1
#define FRAME_UP_AXIS_STATE     0x01
#define FRAME_UP_AUTH_RESPONSE  0x02
#define FRAME_UP_STATUS         0x03

// Downstream: RP2040 #1 → #2
#define FRAME_DN_FORCE_OUTPUT   0x01
#define FRAME_DN_AUTH_CHALLENGE 0x02
#define FRAME_DN_CONFIG         0x03

// ─── Command Flags (downstream) ───

#define CMD_FLAG_STOP_ALL_FFB   (1 << 0)
#define CMD_FLAG_SET_RANGE      (1 << 1)
#define CMD_FLAG_SET_POWER      (1 << 2)

// ─── Connection Status ───

#define G29_STATUS_DISCONNECTED     0
#define G29_STATUS_CONNECTED        1
#define G29_STATUS_MODE_SWITCHING   2

#define AUTH_STATUS_DISCONNECTED    0
#define AUTH_STATUS_CONNECTED       1

// ─── Upstream Frame: G29 State (RP2040 #2 → #1) — 32 bytes ───
// All fields naturally aligned (no float, no misalignment risk)

typedef struct __attribute__((packed)) {
    uint8_t  frame_type;            // 0
    uint8_t  sequence;              // 1
    uint16_t steering;              // 2-3: raw from G29 (LE)
    int16_t  steering_velocity;     // 4-5: Q7.8 fixed-point
    int16_t  steering_accel;        // 6-7: Q7.8 fixed-point
    uint8_t  gas;                   // 8:  0xFF=released, 0x00=pressed
    uint8_t  brake;                 // 9
    uint8_t  clutch;                // 10
    uint8_t  buttons0;              // 11: dpad(3:0) + face(7:4)
    uint8_t  buttons1;              // 12: R1,L1,R2,L2,Share,Options,R3,L3
    uint8_t  buttons2;              // 13: shifter gears + plus
    uint8_t  buttons3;              // 14: minus, dial, PS
    uint8_t  g29_status;            // 15: G29_STATUS_*
    uint8_t  auth_status;           // 16: AUTH_STATUS_*
    uint8_t  error_count;           // 17: wrapping
    uint8_t  reserved[13];          // 18-30: auth response data when type=0x02
    uint8_t  crc;                   // 31
} spi_upstream_frame_t;

_Static_assert(sizeof(spi_upstream_frame_t) == SPI_FRAME_SIZE,
               "Upstream frame must be exactly 32 bytes");

// ─── Downstream Frame: Force + Commands (RP2040 #1 → #2) — 32 bytes ───

typedef struct __attribute__((packed)) {
    uint8_t  frame_type;            // 0
    uint8_t  sequence;              // 1
    int16_t  af_wheel;              // 2-3: Q1.14 fixed-point (-1.0 to 1.0)
    int16_t  af_motor_low;          // 4-5: Q1.14
    int16_t  af_motor_high;         // 6-7: Q1.14
    uint8_t  power_level;           // 8:  0-255
    uint8_t  cmd_flags;             // 9:  CMD_FLAG_*
    uint16_t angle_setting;         // 10-11: degrees (LE)
    uint8_t  reserved[19];          // 12-30: auth challenge data when type=0x02
    uint8_t  crc;                   // 31
} spi_downstream_frame_t;

_Static_assert(sizeof(spi_downstream_frame_t) == SPI_FRAME_SIZE,
               "Downstream frame must be exactly 32 bytes");

// ─── Fixed-Point Conversion Helpers ───

static inline int16_t spi_float_to_force(float f) {
    if (f > 1.999f) f = 1.999f;
    if (f < -2.0f) f = -2.0f;
    return (int16_t)(f * SPI_FORCE_SCALE);
}

static inline float spi_force_to_float(int16_t v) {
    return (float)v / SPI_FORCE_SCALE;
}

static inline int16_t spi_float_to_velocity(float f) {
    if (f > 127.0f) f = 127.0f;
    if (f < -128.0f) f = -128.0f;
    return (int16_t)(f * SPI_VELOCITY_SCALE);
}

static inline float spi_velocity_to_float(int16_t v) {
    return (float)v / SPI_VELOCITY_SCALE;
}

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
