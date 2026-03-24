/**
 * auth_relay.c — PowerA Auth Board Communication (Stub)
 */

#include "auth_relay.h"
#include "bridge_config.h"
#include "pio_usb.h"

#include <stdio.h>

static bool auth_connected = false;

bool auth_relay_init(void) {
    // Add second root port on PIO0 for auth board
    int result = pio_usb_host_add_port(AUTH_USB_DP_PIN, PIO_USB_PINOUT_DPDM);
    if (result < 0) {
        printf("[AUTH] Failed to add root port 1 on GPIO %d/%d\n",
               AUTH_USB_DP_PIN, AUTH_USB_DM_PIN);
        return false;
    }

    printf("[AUTH] Root port 1 added on GPIO %d/%d\n",
           AUTH_USB_DP_PIN, AUTH_USB_DM_PIN);
    return true;
}

bool auth_relay_connected(void) {
    return auth_connected;
}

uint16_t auth_relay_forward(const uint8_t *challenge, uint16_t challenge_len,
                            uint8_t *response, uint16_t response_max) {
    // TODO: Phase 3 — implement actual auth relay
    (void)challenge; (void)challenge_len;
    (void)response; (void)response_max;
    return 0;
}
