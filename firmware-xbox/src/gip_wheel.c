/**
 * gip_wheel.c — Wheel Report Building (Stub)
 */
#include "gip_wheel.h"
#include <string.h>

void gip_wheel_init(void) {}

void gip_wheel_build_state_report(const spi_upstream_frame_t *g29,
                                   uint8_t *out, uint8_t *out_len) {
    (void)g29; (void)out;
    *out_len = 0;
    // TODO: Phase 2/4 — build 0x20 wheel state report
}

void gip_wheel_build_config_report(uint8_t *out, uint8_t *out_len) {
    (void)out;
    *out_len = 0;
    // TODO: Phase 2 — build 0x21 static config report
}

void gip_wheel_handle_request(uint8_t req_type, uint16_t req_data) {
    (void)req_type; (void)req_data;
    // TODO: Phase 2 — handle 0x0A wheel requests
}
