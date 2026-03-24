/**
 * auth_relay.h — PowerA Auth Board Communication
 */
#ifndef AUTH_RELAY_H
#define AUTH_RELAY_H

#include <stdint.h>
#include <stdbool.h>

/** Initialize auth board on PIO0 root port 1 (GPIO 2/3). */
bool auth_relay_init(void);

/** Returns true if auth board is enumerated. */
bool auth_relay_connected(void);

/**
 * Forward an auth challenge to the auth board and wait for response.
 * challenge/response are raw byte buffers.
 * Returns response length, or 0 on timeout/failure.
 * Timeout: 500ms.
 */
uint16_t auth_relay_forward(const uint8_t *challenge, uint16_t challenge_len,
                            uint8_t *response, uint16_t response_max);

#endif // AUTH_RELAY_H
