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
#include <string.h>

// Frame size — fixed, never changes
#define SPI_FRAME_SIZE      32
#define SPI_CRC_OFFSET      31

// SPI bus parameters
#define SPI_CLOCK_HZ        1000000     // 1 MHz (reduced from 10MHz for jumper wire reliability)
#define SPI_FRAME_RATE_HZ   1000        // 1 kHz
#define SPI_TIMEOUT_MS      50          // Error state if no valid frame in 50ms
#define SPI_AUTH_SEQ_TIMEOUT_MS  500    // Multi-frame auth sequence timeout
#define SPI_AUTH_MAX_PAYLOAD    256    // Max auth challenge/response size
#define SPI_AUTH_DN_FRAG_SIZE   16     // Max fragment per downstream frame (reserved[3..18])
#define SPI_AUTH_UP_FRAG_SIZE   10     // Max fragment per upstream frame (reserved[3..12])

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

// ─── Auth Fragment Helpers ───
// Downstream reserved[19] layout when frame_type == FRAME_DN_AUTH_CHALLENGE:
//   [0]    = total_len  (0 = no auth data)
//   [1]    = fragment_offset
//   [2]    = fragment_len (1-16)
//   [3..18] = fragment data
//
// Upstream reserved[13] layout when frame_type == FRAME_UP_AUTH_RESPONSE:
//   [0]    = total_len  (0 = no auth data)
//   [1]    = fragment_offset
//   [2]    = fragment_len (1-10)
//   [3..12] = fragment data

typedef struct {
    uint8_t total_len;          // Total payload size
    uint8_t fragment_offset;    // Offset of this fragment in the full payload
    uint8_t fragment_len;       // Bytes in this fragment
} spi_auth_frag_header_t;

// Pack an auth fragment into a downstream frame's reserved[] field
// Returns bytes of payload packed (0 if nothing to send)
static inline uint8_t spi_auth_pack_downstream(spi_downstream_frame_t *frame,
                                                const uint8_t *payload,
                                                uint8_t total_len,
                                                uint8_t offset) {
    if (offset >= total_len) return 0;
    uint8_t remaining = total_len - offset;
    uint8_t frag_len = remaining < SPI_AUTH_DN_FRAG_SIZE ? remaining : SPI_AUTH_DN_FRAG_SIZE;

    frame->frame_type = FRAME_DN_AUTH_CHALLENGE;
    frame->reserved[0] = total_len;
    frame->reserved[1] = offset;
    frame->reserved[2] = frag_len;
    memcpy(&frame->reserved[3], &payload[offset], frag_len);
    return frag_len;
}

// Unpack an auth fragment from a downstream frame's reserved[] field
// Returns fragment length (0 if no auth data)
static inline uint8_t spi_auth_unpack_downstream(const spi_downstream_frame_t *frame,
                                                  spi_auth_frag_header_t *hdr,
                                                  uint8_t *frag_data,
                                                  uint8_t frag_data_max) {
    if (frame->frame_type != FRAME_DN_AUTH_CHALLENGE) return 0;
    hdr->total_len = frame->reserved[0];
    hdr->fragment_offset = frame->reserved[1];
    hdr->fragment_len = frame->reserved[2];
    if (hdr->total_len == 0 || hdr->fragment_len == 0) return 0;
    if (hdr->fragment_len > SPI_AUTH_DN_FRAG_SIZE) hdr->fragment_len = SPI_AUTH_DN_FRAG_SIZE;
    uint8_t copy_len = hdr->fragment_len < frag_data_max ? hdr->fragment_len : frag_data_max;
    memcpy(frag_data, &frame->reserved[3], copy_len);
    return copy_len;
}

// Pack an auth fragment into an upstream frame's reserved[] field
static inline uint8_t spi_auth_pack_upstream(spi_upstream_frame_t *frame,
                                              const uint8_t *payload,
                                              uint8_t total_len,
                                              uint8_t offset) {
    if (offset >= total_len) return 0;
    uint8_t remaining = total_len - offset;
    uint8_t frag_len = remaining < SPI_AUTH_UP_FRAG_SIZE ? remaining : SPI_AUTH_UP_FRAG_SIZE;

    frame->frame_type = FRAME_UP_AUTH_RESPONSE;
    frame->reserved[0] = total_len;
    frame->reserved[1] = offset;
    frame->reserved[2] = frag_len;
    memcpy(&frame->reserved[3], &payload[offset], frag_len);
    return frag_len;
}

// Unpack an auth fragment from an upstream frame's reserved[] field
static inline uint8_t spi_auth_unpack_upstream(const spi_upstream_frame_t *frame,
                                                spi_auth_frag_header_t *hdr,
                                                uint8_t *frag_data,
                                                uint8_t frag_data_max) {
    if (frame->frame_type != FRAME_UP_AUTH_RESPONSE) return 0;
    hdr->total_len = frame->reserved[0];
    hdr->fragment_offset = frame->reserved[1];
    hdr->fragment_len = frame->reserved[2];
    if (hdr->total_len == 0 || hdr->fragment_len == 0) return 0;
    if (hdr->fragment_len > SPI_AUTH_UP_FRAG_SIZE) hdr->fragment_len = SPI_AUTH_UP_FRAG_SIZE;
    uint8_t copy_len = hdr->fragment_len < frag_data_max ? hdr->fragment_len : frag_data_max;
    memcpy(frag_data, &frame->reserved[3], copy_len);
    return copy_len;
}

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

// ─── CRC-8 (polynomial 0x07, init 0x00, table-based) ───
// 256-byte LUT: ~93 cycles for 31 bytes vs ~2000 cycles bit-by-bit

static const uint8_t spi_crc8_table[256] = {
    0x00, 0x07, 0x0E, 0x09, 0x1C, 0x1B, 0x12, 0x15, 0x38, 0x3F, 0x36, 0x31, 0x24, 0x23, 0x2A, 0x2D,
    0x70, 0x77, 0x7E, 0x79, 0x6C, 0x6B, 0x62, 0x65, 0x48, 0x4F, 0x46, 0x41, 0x54, 0x53, 0x5A, 0x5D,
    0xE0, 0xE7, 0xEE, 0xE9, 0xFC, 0xFB, 0xF2, 0xF5, 0xD8, 0xDF, 0xD6, 0xD1, 0xC4, 0xC3, 0xCA, 0xCD,
    0x90, 0x97, 0x9E, 0x99, 0x8C, 0x8B, 0x82, 0x85, 0xA8, 0xAF, 0xA6, 0xA1, 0xB4, 0xB3, 0xBA, 0xBD,
    0xC7, 0xC0, 0xC9, 0xCE, 0xDB, 0xDC, 0xD5, 0xD2, 0xFF, 0xF8, 0xF1, 0xF6, 0xE3, 0xE4, 0xED, 0xEA,
    0xB7, 0xB0, 0xB9, 0xBE, 0xAB, 0xAC, 0xA5, 0xA2, 0x8F, 0x88, 0x81, 0x86, 0x93, 0x94, 0x9D, 0x9A,
    0x27, 0x20, 0x29, 0x2E, 0x3B, 0x3C, 0x35, 0x32, 0x1F, 0x18, 0x11, 0x16, 0x03, 0x04, 0x0D, 0x0A,
    0x57, 0x50, 0x59, 0x5E, 0x4B, 0x4C, 0x45, 0x42, 0x6F, 0x68, 0x61, 0x66, 0x73, 0x74, 0x7D, 0x7A,
    0x89, 0x8E, 0x87, 0x80, 0x95, 0x92, 0x9B, 0x9C, 0xB1, 0xB6, 0xBF, 0xB8, 0xAD, 0xAA, 0xA3, 0xA4,
    0xF9, 0xFE, 0xF7, 0xF0, 0xE5, 0xE2, 0xEB, 0xEC, 0xC1, 0xC6, 0xCF, 0xC8, 0xDD, 0xDA, 0xD3, 0xD4,
    0x69, 0x6E, 0x67, 0x60, 0x75, 0x72, 0x7B, 0x7C, 0x51, 0x56, 0x5F, 0x58, 0x4D, 0x4A, 0x43, 0x44,
    0x19, 0x1E, 0x17, 0x10, 0x05, 0x02, 0x0B, 0x0C, 0x21, 0x26, 0x2F, 0x28, 0x3D, 0x3A, 0x33, 0x34,
    0x4E, 0x49, 0x40, 0x47, 0x52, 0x55, 0x5C, 0x5B, 0x76, 0x71, 0x78, 0x7F, 0x6A, 0x6D, 0x64, 0x63,
    0x3E, 0x39, 0x30, 0x37, 0x22, 0x25, 0x2C, 0x2B, 0x06, 0x01, 0x08, 0x0F, 0x1A, 0x1D, 0x14, 0x13,
    0xAE, 0xA9, 0xA0, 0xA7, 0xB2, 0xB5, 0xBC, 0xBB, 0x96, 0x91, 0x98, 0x9F, 0x8A, 0x8D, 0x84, 0x83,
    0xDE, 0xD9, 0xD0, 0xD7, 0xC2, 0xC5, 0xCC, 0xCB, 0xE6, 0xE1, 0xE8, 0xEF, 0xFA, 0xFD, 0xF4, 0xF3,
};

static inline uint8_t spi_crc8(const uint8_t *data, uint8_t len) {
    uint8_t crc = 0x00;
    for (uint8_t i = 0; i < len; i++) {
        crc = spi_crc8_table[crc ^ data[i]];
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
