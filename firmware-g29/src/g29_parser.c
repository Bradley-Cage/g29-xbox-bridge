/**
 * g29_parser.c — G29 HID Report Parser (Legacy DFGT Mode, PID 0xC294)
 *
 * Legacy DFGT report layout (27 bytes, Report ID stripped):
 *   Byte 0:    Buttons low  — Cross(4) Square(5) Circle(6) Triangle(7)
 *                             lower nibble: additional buttons TBD
 *   Byte 1:    Buttons high — R1(0) L1(1) R2(2) L2(3) Share(4) Options(5) R3(6) L3(7)
 *   Byte 2:    D-pad / hat switch (low nibble, 0x08 = neutral)
 *   Byte 3:    Steering 8-bit (0x00=left, 0x80=center, 0xFF=right)
 *   Byte 4:    Combined gas/brake axis (0x7F=neutral, 0x00=gas, 0xFF=brake) — unused
 *   Byte 5:    Unknown axis (0x7F center)
 *   Byte 6:    Unknown axis (0x7F center)
 *   Bytes 7-16:  Zeros / unknown
 *   Byte 17:   Brake  separate (0x00=released, 0xFF=pressed)
 *   Byte 18:   Gas    separate (0x00=released, 0xFF=pressed)
 *   Bytes 19-26: Zeros / unknown
 *
 * Pedal note: Legacy raw values are 0x00=released, 0xFF=pressed.
 * The rest of the pipeline expects inverted convention (0xFF=released, 0x00=pressed)
 * so we invert here.
 *
 * Steering note: Only 8-bit in legacy mode. We scale to 16-bit (x * 257) so the
 * rest of the pipeline sees a consistent uint16_t range. Full 16-bit resolution
 * will be available after mode switch to native G29 (PID 0xC24F).
 */

#include "g29_parser.h"
#include "bridge_config.h"
#include "pico/time.h"

#include <string.h>

bool g29_parse_report(const uint8_t *data, uint16_t len, g29_state_t *state) {
    if (len < G29_REPORT_SIZE_LEGACY) {
        state->valid = false;
        return false;
    }

    // ── Byte 0: Face buttons (upper nibble) ──
    // Lower nibble bits are unknown/additional buttons in DFGT mode
    state->btn_cross    = (data[0] & 0x10) != 0;
    state->btn_square   = (data[0] & 0x20) != 0;
    state->btn_circle   = (data[0] & 0x40) != 0;
    state->btn_triangle = (data[0] & 0x80) != 0;

    // ── Byte 1: Shoulders + menu buttons ──
    state->btn_r1       = (data[1] & 0x01) != 0;  // Right paddle
    state->btn_l1       = (data[1] & 0x02) != 0;  // Left paddle
    state->btn_r2       = (data[1] & 0x04) != 0;
    state->btn_l2       = (data[1] & 0x08) != 0;
    state->btn_share    = (data[1] & 0x10) != 0;
    state->btn_options  = (data[1] & 0x20) != 0;
    state->btn_r3       = (data[1] & 0x40) != 0;
    state->btn_l3       = (data[1] & 0x80) != 0;

    // ── Byte 2: D-pad / hat switch (low nibble) ──
    state->dpad = data[2] & 0x0F;
    if (state->dpad > G29_DPAD_NONE) {
        state->dpad = G29_DPAD_NONE;
    }

    // ── Byte 3: Steering (8-bit) ──
    // Scale 0x00-0xFF to 0x0000-0xFFFF so downstream sees uint16_t range.
    // Multiply by 257 (0x101): e.g. 0x80 -> 0x8080, 0xFF -> 0xFFFF, 0x00 -> 0x0000
    state->steering = (uint16_t)data[3] * 257;

    // ── Bytes 17-18: Separate pedal axes ──
    // Raw: 0x00 = released, 0xFF = fully pressed
    // Pipeline expects inverted: 0xFF = released, 0x00 = fully pressed
    state->brake = 0xFF - data[17];
    state->gas   = 0xFF - data[18];

    // ── Clutch: not available in legacy DFGT mode ──
    state->clutch = G29_PEDAL_RELEASED;

    // ── Shifter / misc buttons: not available in legacy DFGT mode ──
    state->shifter_gear   = 0;
    state->btn_plus       = false;
    state->btn_minus      = false;
    state->btn_dial_right = false;
    state->btn_dial_left  = false;
    state->btn_enter      = false;
    state->btn_ps         = false;

    state->valid = true;
    state->timestamp_us = time_us_64();
    return true;
}
