/**
 * g29_parser.h — G29 HID Report Parser (PS3 Mode)
 */
#ifndef G29_PARSER_H
#define G29_PARSER_H

#include <stdint.h>
#include <stdbool.h>

/** Parsed G29 input state. */
typedef struct {
    // Axes
    uint16_t steering;      // 0x0000 (full left) to 0xFFFF (full right)
    uint8_t  gas;           // 0xFF=released, 0x00=pressed
    uint8_t  brake;
    uint8_t  clutch;

    // D-pad (raw hat switch value 0-8)
    uint8_t  dpad;

    // Face buttons (from byte 0 high nibble)
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

    // Byte 2: shifter
    uint8_t shifter_gear;   // 0=neutral, 1-6=gears, 0x40=reverse
    bool btn_plus;

    // Byte 3: misc
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
 * Parse a raw G29 HID report (PS3 mode, 12 bytes) into structured state.
 * Returns true if the report was valid.
 */
bool g29_parse_report(const uint8_t *data, uint8_t len, g29_state_t *state);

#endif // G29_PARSER_H
