/**
 * auth_host.h — PowerA PIO-USB Host + Auth Relay (Board X local)
 *
 * Hosts an intact PowerA Xbox controller via PIO-USB on GPIO 0/1.
 * Acts as minimal GIP host, relays auth challenges/responses directly
 * between gip_protocol.c (Xbox side) and the PowerA's security chip.
 * No SPI needed — everything stays on Board X.
 */
#ifndef AUTH_HOST_H
#define AUTH_HOST_H

#include <stdint.h>
#include <stdbool.h>

// PIO-USB pin for PowerA controller (Board X, DWEII RP2040-Zero)
#define AUTH_PIO_USB_DP_PIN     0   // GPIO 0 = D+
#define AUTH_PIO_USB_DM_PIN     1   // GPIO 1 = D-

// GIP interface class for Xbox controllers
#define GIP_IFACE_CLASS         0xFF
#define GIP_IFACE_SUBCLASS      0x47
#define GIP_IFACE_PROTOCOL      0xD0

// GIP host commands
#define GIP_HOST_CMD_ANNOUNCE       0x02
#define GIP_HOST_CMD_STATUS         0x03
#define GIP_HOST_CMD_IDENTIFY       0x04
#define GIP_HOST_CMD_POWER          0x05
#define GIP_HOST_CMD_AUTHENTICATE   0x06
#define GIP_HOST_CMD_GUIDE          0x07
#define GIP_HOST_CMD_INPUT          0x20

// GIP host flags
#define GIP_HOST_FLAG_CHUNKED       0x80
#define GIP_HOST_FLAG_CHUNK_START   0x40

// GIP power states
#define GIP_HOST_STATE_ON           0x00

/** Initialize PIO-USB host on GPIO 0/1 for PowerA. Call from Core 1. */
bool auth_host_init(void);

/**
 * Run PIO-USB host task + GIP state machine. Call from Core 1 loop.
 * Non-blocking — handles device enumeration, GIP startup, and
 * pending auth challenge/response transfers.
 */
void auth_host_task(void);

/** Returns true if PowerA is GIP-ready for auth relay. */
bool auth_host_connected(void);

/**
 * Queue an auth challenge for the PowerA controller.
 * Called from Core 0 (gip_protocol.c context).
 * Returns true if accepted.
 */
bool auth_host_send_challenge(const uint8_t *challenge, uint16_t len);

/**
 * Retrieve completed auth response.
 * Called from Core 0 (main loop context).
 * Returns response length (0 if not ready).
 */
uint16_t auth_host_get_response(uint8_t *response, uint16_t max_len);

#endif // AUTH_HOST_H
