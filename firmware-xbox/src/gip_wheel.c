/**
 * gip_wheel.c — Xbox GIP Wheel Report Building & Request Handling
 *
 * Translates G29 input data (received over SPI) into Xbox GIP wheel reports:
 *   0x20 — Wheel State Report (8 bytes, sent every frame)
 *   0x21 — Wheel Config Report (6 bytes, sent once on request)
 *
 * Also handles incoming wheel requests (command 0x0A subtypes).
 *
 * Button mapping (G29 PlayStation -> Xbox):
 *   Cross    -> A          Square   -> X
 *   Circle   -> B          Triangle -> Y
 *   L1       -> LB + Paddle Left    R1 -> RB + Paddle Right
 *   Share    -> View       Options  -> Menu
 *   PS       -> Guide
 *
 * Pedal mapping:
 *   G29 gas/brake are 0xFF=released, 0x00=pressed (8-bit).
 *   Xbox expects 0-1023 (10-bit), 0=released, 1023=fully pressed.
 *   Conversion: xbox_val = (uint16_t)(0xFF - g29_val) * 1023 / 255
 *
 * Steering:
 *   SPI frame carries uint16_t 0x0000-0xFFFF (full left to full right).
 *   Xbox expects the same range, so it passes through directly.
 */

#include "gip_wheel.h"
#include "gip_protocol.h"
#include "spi_protocol.h"

#include <string.h>
#include <stdio.h>

// ─── G29 SPI Button Bit Definitions ───
// buttons0: dpad(3:0) | Cross(4) | Square(5) | Circle(6) | Triangle(7)
#define G29_BTN0_CROSS      0x10
#define G29_BTN0_SQUARE     0x20
#define G29_BTN0_CIRCLE     0x40
#define G29_BTN0_TRIANGLE   0x80
#define G29_BTN0_DPAD_MASK  0x0F

// buttons1: R1(0) | L1(1) | R2(2) | L2(3) | Share(4) | Options(5) | R3(6) | L3(7)
#define G29_BTN1_R1         0x01
#define G29_BTN1_L1         0x02
#define G29_BTN1_R2         0x04
#define G29_BTN1_L2         0x08
#define G29_BTN1_SHARE      0x10
#define G29_BTN1_OPTIONS    0x20
#define G29_BTN1_R3         0x40
#define G29_BTN1_L3         0x80

// buttons3: Minus(0) | DialR(1) | DialL(2) | Enter(3) | PS(4)
#define G29_BTN3_PS         0x10

// ─── Xbox GIP Wheel State Report Bit Definitions (0x20) ───
// Byte 0: Buttons 1
#define XBOX_BTN0_A         0x01
#define XBOX_BTN0_B         0x02
#define XBOX_BTN0_X         0x04
#define XBOX_BTN0_Y         0x08
#define XBOX_BTN0_LB        0x10
#define XBOX_BTN0_RB        0x20
#define XBOX_BTN0_VIEW      0x40
#define XBOX_BTN0_MENU      0x80

// Byte 1: Buttons 2
#define XBOX_BTN1_GUIDE     0x01
#define XBOX_BTN1_PADDLE_L  0x04
#define XBOX_BTN1_PADDLE_R  0x08

// ─── GIP Wheel State Report Layout ───
#define GIP_WHEEL_STATE_SIZE    8

// ─── GIP Wheel Config Report Layout ───
#define GIP_WHEEL_CONFIG_SIZE   6
#define WHEEL_TYPE_STANDARD     0x01
#define WHEEL_SUBTYPE_STANDARD  0x01
#define WHEEL_MAX_ROTATION      900     // G29 max rotation in degrees
#define WHEEL_NUM_BUTTONS       12      // A,B,X,Y,LB,RB,View,Menu,Guide,PaddleL,PaddleR + d-pad
#define WHEEL_FLAG_FFB          0x01
#define WHEEL_FLAG_CLUTCH       0x00    // G29 clutch not exposed over GIP

// ─── D-pad Translation ───
// G29 hat switch values: 0=N, 1=NE, 2=E, 3=SE, 4=S, 5=SW, 6=W, 7=NW, 8=neutral
// Xbox d-pad is embedded in the button bytes. For GIP wheels, the d-pad state
// is part of the buttons byte. We encode it in bits 0-3 of xbox_buttons0 before
// the face buttons. However, looking at the spec more carefully, the 0x20 format
// doesn't have separate d-pad bits — it's just buttons. Xbox wheels typically
// don't have d-pads, but the G29 does. We'll map d-pad to the first 4 bits
// as: Up=bit0, Down=bit1, Left=bit2, Right=bit3 — but this overlaps with A/B/X/Y.
//
// Actually, the spec says byte 0 is ONLY button bits (A,B,X,Y,LB,RB,View,Menu)
// and byte 1 is (Guide, PaddleL, PaddleR). D-pad is NOT part of the standard
// wheel state report. So we skip d-pad in the 0x20 report.

// ─── External: get latest SPI frame for initial request ───
extern bool spi_master_get_rx_frame(spi_upstream_frame_t *out);

// ─── Pedal Scaling ───
// G29:  0xFF=released, 0x00=pressed
// Xbox: 0=released, 1023=fully pressed (uint16_t LE)
static inline uint16_t scale_pedal_to_xbox(uint8_t g29_pedal) {
    // Invert: 0xFF->0, 0x00->255
    uint16_t inverted = (uint16_t)(0xFF - g29_pedal);
    // Scale 0-255 -> 0-1023: multiply by 1023, divide by 255
    // Use (inverted * 4) + correction for better rounding:
    //   1023/255 = 4.0118..., so val*4 + val/64 is close but let's be exact.
    return (uint16_t)((uint32_t)inverted * 1023 / 255);
}

// ─── Button Mapping ───
static void map_buttons(const spi_upstream_frame_t *g29,
                         uint8_t *xbox_btn0, uint8_t *xbox_btn1) {
    uint8_t b0 = 0;
    uint8_t b1 = 0;

    // Face buttons: Cross->A, Circle->B, Square->X, Triangle->Y
    if (g29->buttons0 & G29_BTN0_CROSS)    b0 |= XBOX_BTN0_A;
    if (g29->buttons0 & G29_BTN0_CIRCLE)   b0 |= XBOX_BTN0_B;
    if (g29->buttons0 & G29_BTN0_SQUARE)   b0 |= XBOX_BTN0_X;
    if (g29->buttons0 & G29_BTN0_TRIANGLE) b0 |= XBOX_BTN0_Y;

    // Shoulders: L1->LB+PaddleL, R1->RB+PaddleR
    // G29 paddle shifters are L1/R1. Map to both Xbox bumpers and paddles
    // since Xbox games may check either for gear shifts.
    if (g29->buttons1 & G29_BTN1_L1) {
        b0 |= XBOX_BTN0_LB;
        b1 |= XBOX_BTN1_PADDLE_L;
    }
    if (g29->buttons1 & G29_BTN1_R1) {
        b0 |= XBOX_BTN0_RB;
        b1 |= XBOX_BTN1_PADDLE_R;
    }

    // Menu buttons: Share->View, Options->Menu
    if (g29->buttons1 & G29_BTN1_SHARE)    b0 |= XBOX_BTN0_VIEW;
    if (g29->buttons1 & G29_BTN1_OPTIONS)  b0 |= XBOX_BTN0_MENU;

    // Guide: PS->Guide
    if (g29->buttons3 & G29_BTN3_PS)       b1 |= XBOX_BTN1_GUIDE;

    *xbox_btn0 = b0;
    *xbox_btn1 = b1;
}

// ─── Public API ───

void gip_wheel_init(void) {
    printf("[WHEEL] GIP wheel report module initialized\n");
}

void gip_wheel_build_state_report(const spi_upstream_frame_t *g29,
                                   uint8_t *out, uint8_t *out_len) {
    // 0x20 Wheel State Report — 8 bytes
    // [0]   Buttons 1 (A,B,X,Y,LB,RB,View,Menu)
    // [1]   Buttons 2 (Guide,PaddleL,PaddleR)
    // [2-3] Steering (uint16_t LE, 0=left, 0x7FFF=center, 0xFFFF=right)
    // [4-5] Left trigger / brake (uint16_t LE, 0-1023)
    // [6-7] Right trigger / gas (uint16_t LE, 0-1023)

    uint8_t xbox_btn0, xbox_btn1;
    map_buttons(g29, &xbox_btn0, &xbox_btn1);

    uint16_t steering = g29->steering;
    uint16_t brake = scale_pedal_to_xbox(g29->brake);
    uint16_t gas   = scale_pedal_to_xbox(g29->gas);

    out[0] = xbox_btn0;
    out[1] = xbox_btn1;
    out[2] = (uint8_t)(steering & 0xFF);        // Steering LE low
    out[3] = (uint8_t)(steering >> 8);           // Steering LE high
    out[4] = (uint8_t)(brake & 0xFF);            // Brake LE low
    out[5] = (uint8_t)(brake >> 8);              // Brake LE high
    out[6] = (uint8_t)(gas & 0xFF);              // Gas LE low
    out[7] = (uint8_t)(gas >> 8);                // Gas LE high

    *out_len = GIP_WHEEL_STATE_SIZE;
}

void gip_wheel_build_config_report(uint8_t *out, uint8_t *out_len) {
    // 0x21 Wheel Config Report — 6 bytes (static capabilities)
    // [0]   Device type (0x01 = wheel)
    // [1]   Subtype (0x01 = standard wheel)
    // [2-3] Max rotation angle (uint16_t LE, 900 degrees)
    // [4]   Number of buttons
    // [5]   Flags (0x01 = supports FFB)

    out[0] = WHEEL_TYPE_STANDARD;
    out[1] = WHEEL_SUBTYPE_STANDARD;
    out[2] = (uint8_t)(WHEEL_MAX_ROTATION & 0xFF);     // 900 LE low = 0x84
    out[3] = (uint8_t)(WHEEL_MAX_ROTATION >> 8);        // 900 LE high = 0x03
    out[4] = WHEEL_NUM_BUTTONS;
    out[5] = WHEEL_FLAG_FFB;

    *out_len = GIP_WHEEL_CONFIG_SIZE;
}

void gip_wheel_handle_request(uint8_t req_type, uint16_t req_data) {
    uint8_t report[8];
    uint8_t report_len;

    switch (req_type) {
        case GIP_WHEEL_REQ_INITIAL: {
            printf("[WHEEL] Initial request: sending config + state\n");

            // Send 0x21 config report first
            gip_wheel_build_config_report(report, &report_len);
            gip_send_config_report(report, report_len);

            // Send 0x20 state report with current wheel state
            spi_upstream_frame_t frame;
            if (spi_master_get_rx_frame(&frame) &&
                frame.g29_status == G29_STATUS_CONNECTED) {
                gip_wheel_build_state_report(&frame, report, &report_len);
            } else {
                // No valid G29 data yet — send centered/released defaults
                memset(report, 0, sizeof(report));
                report[2] = 0xFF;   // Steering center low byte (0x7FFF)
                report[3] = 0x7F;   // Steering center high byte
                report_len = GIP_WHEEL_STATE_SIZE;
            }
            gip_send_input_report(report, report_len);
            break;
        }

        case GIP_WHEEL_REQ_ANGLE:
            printf("[WHEEL] Angle request: %d degrees\n", req_data);
            // TODO: Forward to RP2040 #2 via SPI CMD_FLAG_SET_RANGE
            break;

        case GIP_WHEEL_REQ_POWER:
            printf("[WHEEL] Power request: %d\n", req_data & 0xFF);
            // TODO: Forward to RP2040 #2 via SPI CMD_FLAG_SET_POWER
            break;

        case GIP_WHEEL_REQ_VIBRATION:
            printf("[WHEEL] Vibration request: L=%d R=%d\n",
                   req_data & 0xFF, (req_data >> 8) & 0xFF);
            // Simple vibration is handled outside the Equation VM
            // TODO: Forward to RP2040 #2 via SPI
            break;

        default:
            printf("[WHEEL] Unknown request type: 0x%02X data=0x%04X\n",
                   req_type, req_data);
            break;
    }
}
