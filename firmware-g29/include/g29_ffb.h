/**
 * g29_ffb.h — G29 Force Feedback Command Builders (Logitech Legacy Protocol)
 */
#ifndef G29_FFB_H
#define G29_FFB_H

#include <stdint.h>
#include <stdbool.h>

/** Initialize FFB state. Sends stop-all and disable-autocenter. */
void g29_ffb_init(void);

/**
 * Set constant force on slot 0.
 * af: -1.0 (full left) to 1.0 (full right), 0.0 = no force.
 * Handles play vs update automatically.
 */
void g29_ffb_set_constant_force(float af);

/** Stop all effects on all slots. */
void g29_ffb_stop_all(void);

/** Set wheel rotation range in degrees (40-900). */
void g29_ffb_set_range(uint16_t degrees);

/**
 * FFB update tick — call at 250Hz (every 4ms).
 * Sends queued force value to G29 if changed.
 */
void g29_ffb_tick(void);

#endif // G29_FFB_H
