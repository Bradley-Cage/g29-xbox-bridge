/**
 * g29_parser.h — G29 HID Report Parser (Legacy DFGT Mode, PID 0xC294)
 *
 * In legacy mode the G29 presents as a Driving Force GT with 27-byte reports.
 * Steering is only 8-bit until the mode switch to native G29 (PID 0xC24F).
 */
#ifndef G29_PARSER_H
#define G29_PARSER_H

#include <stdint.h>
#include <stdbool.h>

/** Parsed G29 input state. */
typedef struct {
    // Axes
    uint16_t steering;      // 0x0000 (full left) to 0xFFFF (full right)
                            // Legacy: 8-bit source scaled to 16-bit
    uint8_t  gas;           // 0xFF=released, 0x00=pressed (inverted from raw)
    uint8_t  brake;         // 0xFF=released, 0x00=pressed (inverted from raw)
    uint8_t  clutch;        // Unused in legacy mode (always 0xFF = released)

    // D-pad (raw hat switch value 0-8)
    uint8_t  dpad;

    // Face buttons (from byte 0 upper nibble in legacy mode)
    bool btn_cross;
    bool btn_square;
    bool btn_circle;
    bool btn_triangle;

    // Shoulders / paddles (from byte 1)
    bool btn_r1;            // Right paddle
    bool btn_l1;            // Left paddle
    bool btn_r2;
    bool btn_l2;
    bool btn_share;
    bool btn_options;
    bool btn_r3;
    bool btn_l3;

    // Shifter (not available in legacy mode — zeroed)
    uint8_t shifter_gear;
    bool btn_plus;

    // Misc (not available in legacy mode — zeroed)
    bool btn_minus;
    bool btn_dial_right;
    bool btn_dial_left;
    bool btn_enter;
    bool btn_ps;

    // Metadata
    bool valid;
    uint64_t timestamp_us;
} g29_state_t;

/**
 * Parse a raw G29 HID report into structured state.
 * Supports legacy DFGT mode (27 bytes, PID 0xC294).
 * Returns true if the report was valid.
 */
bool g29_parse_report(const uint8_t *data, uint16_t len, g29_state_t *state);

#endif // G29_PARSER_H
