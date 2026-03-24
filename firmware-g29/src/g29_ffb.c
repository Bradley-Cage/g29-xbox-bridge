/**
 * g29_ffb.c — G29 Force Feedback (Logitech Legacy Protocol)
 */

#include "g29_ffb.h"
#include "g29_host.h"
#include "bridge_config.h"

#include <string.h>
#include <stdio.h>

static bool slot0_active = false;
static float pending_force = 0.0f;
static bool force_dirty = false;

void g29_ffb_init(void) {
    // Stop all effects
    uint8_t cmd_stop[] = G29_CMD_STOP_ALL;
    g29_send_ffb(cmd_stop);

    // Disable autocenter
    uint8_t cmd_ac[] = G29_CMD_AUTOCENTER_OFF;
    g29_send_ffb(cmd_ac);

    // Set range to 900 degrees
    uint8_t cmd_range[] = G29_CMD_RANGE_900;
    g29_send_ffb(cmd_range);

    slot0_active = false;
    pending_force = 0.0f;
    force_dirty = false;

    printf("[FFB] Initialized: stop all, autocenter off, range 900\n");
}

void g29_ffb_set_constant_force(float af) {
    pending_force = af;
    force_dirty = true;
}

void g29_ffb_stop_all(void) {
    uint8_t cmd[] = G29_CMD_STOP_ALL;
    g29_send_ffb(cmd);
    slot0_active = false;
    pending_force = 0.0f;
    force_dirty = false;
    printf("[FFB] Stop all\n");
}

void g29_ffb_set_range(uint16_t degrees) {
    if (degrees < 40) degrees = 40;
    if (degrees > 900) degrees = 900;

    uint8_t cmd[7] = {0xF8, 0x81,
                      (uint8_t)(degrees & 0xFF),
                      (uint8_t)((degrees >> 8) & 0xFF),
                      0x00, 0x00, 0x00};
    g29_send_ffb(cmd);
    printf("[FFB] Range set to %d degrees\n", degrees);
}

void g29_ffb_tick(void) {
    if (!force_dirty) return;
    if (!g29_is_connected()) return;

    // Convert Af (-1.0 to 1.0) to G29 force byte (0x00=full left, 0x80=center, 0xFF=full right)
    float clamped = pending_force;
    if (clamped < -1.0f) clamped = -1.0f;
    if (clamped > 1.0f) clamped = 1.0f;

    int16_t scaled = (int16_t)(clamped * 127.0f);
    uint8_t g29_force = (uint8_t)(scaled + 128);

    uint8_t op = slot0_active ? FFB_OP_UPDATE : FFB_OP_PLAY;
    uint8_t cmd[7] = {
        (uint8_t)(FFB_SLOT_0 | op),     // Byte 0: slot 0 + play/update
        FFB_TYPE_CONSTANT,               // Byte 1: constant force
        g29_force,                       // Byte 2: force value (slot 0)
        0x00, 0x00, 0x00, 0x00
    };

    if (g29_send_ffb(cmd)) {
        slot0_active = true;
        force_dirty = false;
    }
}
