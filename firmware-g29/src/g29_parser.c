/**
 * g29_parser.c — G29 HID Report Parser (PS3 Mode, Verified Byte Map)
 *
 * Report layout (12 bytes, Report ID stripped):
 *   Byte 0:  D-pad(3:0) + Cross(4) Square(5) Circle(6) Triangle(7)
 *   Byte 1:  R1(0) L1(1) R2(2) L2(3) Share(4) Options(5) R3(6) L3(7)
 *   Byte 2:  Gear 1-6(0-5) Reverse(6) Plus(7)
 *   Byte 3:  Minus(0) DialR(1) DialL(2) Enter(3) PS(4)
 *   Byte 4-5: Steering 16-bit LE (0x0000=left, 0x8000=center, 0xFFFF=right)
 *   Byte 6:  Gas   (0xFF=released, 0x00=pressed)
 *   Byte 7:  Brake (same)
 *   Byte 8:  Clutch (same)
 *   Byte 9-11: H-shifter analog (unused)
 */

#include "g29_parser.h"
#include "bridge_config.h"
#include "pico/time.h"

#include <string.h>

bool g29_parse_report(const uint8_t *data, uint8_t len, g29_state_t *state) {
    if (len < G29_REPORT_SIZE) {
        state->valid = false;
        return false;
    }

    // ── Byte 0: D-pad + face buttons ──
    state->dpad         = data[0] & 0x0F;
    if (state->dpad > G29_DPAD_NONE) {
        state->dpad = G29_DPAD_NONE;
    }
    state->btn_cross    = (data[0] & 0x10) != 0;
    state->btn_square   = (data[0] & 0x20) != 0;
    state->btn_circle   = (data[0] & 0x40) != 0;
    state->btn_triangle = (data[0] & 0x80) != 0;

    // ── Byte 1: Shoulders + menu ──
    state->btn_r1       = (data[1] & 0x01) != 0;  // Right paddle
    state->btn_l1       = (data[1] & 0x02) != 0;  // Left paddle
    state->btn_r2       = (data[1] & 0x04) != 0;
    state->btn_l2       = (data[1] & 0x08) != 0;
    state->btn_share    = (data[1] & 0x10) != 0;
    state->btn_options  = (data[1] & 0x20) != 0;
    state->btn_r3       = (data[1] & 0x40) != 0;
    state->btn_l3       = (data[1] & 0x80) != 0;

    // ── Byte 2: Shifter + plus ──
    state->shifter_gear = data[2] & 0x7F;
    state->btn_plus     = (data[2] & 0x80) != 0;

    // ── Byte 3: Misc buttons ──
    state->btn_minus      = (data[3] & 0x01) != 0;
    state->btn_dial_right = (data[3] & 0x02) != 0;
    state->btn_dial_left  = (data[3] & 0x04) != 0;
    state->btn_enter      = (data[3] & 0x08) != 0;
    state->btn_ps         = (data[3] & 0x10) != 0;

    // ── Bytes 4-5: Steering (16-bit LE) ──
    state->steering = (uint16_t)data[4] | ((uint16_t)data[5] << 8);

    // ── Bytes 6-8: Pedals (inverted: 0xFF=released, 0x00=pressed) ──
    state->gas    = data[6];
    state->brake  = data[7];
    state->clutch = data[8];

    state->valid = true;
    state->timestamp_us = time_us_64();
    return true;
}
