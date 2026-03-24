/**
 * gip_protocol.c — GIP Protocol State Machine (Stub)
 */
#include "gip_protocol.h"
#include <string.h>
#include <stdio.h>

static gip_state_t state;

void gip_init(void) {
    memset(&state, 0, sizeof(state));
    printf("[GIP] State machine initialized\n");
}

void gip_task(void) {
    // TODO: Phase 2 — process incoming GIP commands from Xbox
}

void gip_send_input_report(const uint8_t *payload, uint8_t len) {
    (void)payload; (void)len;
    // TODO: Phase 2 — build GIP frame and send via TinyUSB vendor endpoint
}

void gip_send_hello(void) {
    // TODO: Phase 2 — send GIP Hello Device (0x02)
}

uint8_t gip_next_seq(uint8_t command) {
    uint8_t seq = ++state.seq_counters[command];
    if (seq == 0) seq = ++state.seq_counters[command]; // Skip 0, wrap 1-255
    return seq;
}
