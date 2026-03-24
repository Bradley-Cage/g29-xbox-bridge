/**
 * g29_host.h — G29 PIO USB Host Management
 */
#ifndef G29_HOST_H
#define G29_HOST_H

#include <stdint.h>
#include <stdbool.h>

/** Initialize PIO USB host on PIO0, root port 0 (GPIO 0/1). */
bool g29_host_init(void);

/**
 * Run one iteration of the USB host task.
 * Handles enumeration, mode switch, polling.
 * Returns true if a new HID report was received.
 */
bool g29_host_task(void);

/** Send a 7-byte FFB command to the G29's interrupt OUT endpoint. */
bool g29_send_ffb(const uint8_t cmd[7]);

/** Returns true if G29 is enumerated and in native mode. */
bool g29_is_connected(void);

/** Returns the current connection status code. */
uint8_t g29_get_status(void);

#endif // G29_HOST_H
