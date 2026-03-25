/*
 * DEPRECATED: Auth relay moved to Board X (firmware-xbox/src/auth_host.c).
 * This file is not compiled. It remains on disk for reference only.
 */

/**
 * auth_relay.h — PowerA Controller Auth Passthrough (GIP Host)
 *
 * Manages an intact PowerA Xbox controller on PIO-USB port 1 (GPIO 2/3).
 * Acts as a minimal GIP host: handles device startup, then relays
 * GIP 0x06 auth packets transparently between the Xbox (via SPI)
 * and the PowerA's internal security chip.
 *
 * The PowerA controller is NOT disassembled — it plugs in via USB-A
 * breakout board. Auth payloads are opaque blobs; no crypto on our side.
 */
#ifndef AUTH_RELAY_H
#define AUTH_RELAY_H

#include <stdint.h>
#include <stdbool.h>

/** Initialize auth relay on PIO0 root port 1 (GPIO 2/3). */
bool auth_relay_init(void);

/**
 * Run periodic GIP host tasks. Call every main loop iteration.
 * Handles: GIP packet reception from PowerA, startup state machine,
 * pending auth response polling. Non-blocking.
 */
void auth_relay_task(void);

/** Returns true if PowerA is enumerated and GIP-ready for auth relay. */
bool auth_relay_connected(void);

/**
 * Forward an auth challenge to the PowerA controller via GIP 0x06.
 * Non-blocking — queues the challenge for transmission.
 * The response arrives asynchronously via auth_relay_task() and is
 * retrieved with auth_relay_get_response().
 *
 * Returns true if challenge was accepted for sending.
 */
bool auth_relay_send_challenge(const uint8_t *challenge, uint16_t len);

/**
 * Check for and retrieve a completed auth response.
 * Returns response length (0 if no response ready yet).
 * Clears the response buffer after retrieval.
 */
uint16_t auth_relay_get_response(uint8_t *response, uint16_t response_max);

/** Get the PowerA controller's VID (0 if not yet enumerated). */
uint16_t auth_relay_get_vid(void);

/** Get the PowerA controller's PID (0 if not yet enumerated). */
uint16_t auth_relay_get_pid(void);

/**
 * Device mount/unmount notifications — called from g29_host.c's
 * tuh_mount_cb/tuh_umount_cb for non-G29 devices.
 */
void auth_relay_on_mount(uint8_t dev_addr, uint16_t vid, uint16_t pid);
void auth_relay_on_umount(uint8_t dev_addr);

#endif // AUTH_RELAY_H
