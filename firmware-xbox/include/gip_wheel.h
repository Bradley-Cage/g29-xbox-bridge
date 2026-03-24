/**
 * gip_wheel.h — Xbox GIP Wheel Report Building
 */
#ifndef GIP_WHEEL_H
#define GIP_WHEEL_H

#include <stdint.h>
#include <stdbool.h>
#include "spi_protocol.h"

void gip_wheel_init(void);
void gip_wheel_build_state_report(const spi_upstream_frame_t *g29, uint8_t *out, uint8_t *out_len);
void gip_wheel_build_config_report(uint8_t *out, uint8_t *out_len);
void gip_wheel_handle_request(uint8_t req_type, uint16_t req_data);

#endif
