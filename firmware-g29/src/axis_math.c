/**
 * axis_math.c — Steering Velocity and Acceleration Computation
 *
 * Computes first and second derivatives of the steering position
 * from 1kHz position samples. Uses a simple finite difference
 * with low-pass filtering to reduce noise.
 *
 * Velocity (Av): rate of change of normalized position per second
 *   Av = (pos[n] - pos[n-1]) / dt
 *   where pos is normalized to -1.0..1.0 and dt = 0.001s (1kHz)
 *
 * Acceleration (Aa): rate of change of velocity per second
 *   Aa = (Av[n] - Av[n-1]) / dt
 *
 * Both are filtered with a simple exponential moving average (EMA)
 * to reduce jitter from quantization noise:
 *   filtered = alpha * new_value + (1 - alpha) * filtered
 *   alpha = 0.1 (heavy smoothing, ~10-sample time constant)
 */

#include "bridge_config.h"
#include "spi_protocol.h"
#include "pico/stdlib.h"

#include <stdint.h>

// State
static float prev_position = 0.0f;      // Previous normalized position
static float prev_velocity = 0.0f;      // Previous velocity for accel calc
static float filtered_velocity = 0.0f;  // EMA-filtered velocity
static float filtered_accel = 0.0f;     // EMA-filtered acceleration
static bool first_sample = true;
static uint32_t prev_time_us = 0;       // Timestamp of previous sample

// EMA filter coefficient (0.0-1.0, lower = smoother, higher = more responsive)
#define AXIS_FILTER_ALPHA   0.15f

// Minimum dt to prevent division by zero / noise spikes (0.5ms)
#define AXIS_MIN_DT         0.0005f
// Maximum dt to clamp stale samples (50ms = if no report for 50ms, clamp)
#define AXIS_MAX_DT         0.05f

void axis_math_reset(void) {
    prev_position = 0.0f;
    prev_velocity = 0.0f;
    filtered_velocity = 0.0f;
    filtered_accel = 0.0f;
    first_sample = true;
    prev_time_us = 0;
}

void axis_math_update(uint16_t raw_steering,
                      int16_t *out_velocity_q8,
                      int16_t *out_accel_q8) {
    // Normalize steering: 0x0000(-1.0) → 0x8000(0.0) → 0xFFFF(+1.0)
    float position = ((float)raw_steering / 32768.0f) - 1.0f;

    uint32_t now_us = to_us_since_boot(get_absolute_time());

    if (first_sample) {
        prev_position = position;
        prev_velocity = 0.0f;
        prev_time_us = now_us;
        first_sample = false;
        *out_velocity_q8 = 0;
        *out_accel_q8 = 0;
        return;
    }

    // Dynamic dt from actual elapsed time between reports
    float dt = (float)(now_us - prev_time_us) / 1000000.0f;
    if (dt < AXIS_MIN_DT) dt = AXIS_MIN_DT;
    if (dt > AXIS_MAX_DT) dt = AXIS_MAX_DT;
    prev_time_us = now_us;

    // Raw velocity (change per second)
    float raw_velocity = (position - prev_position) / dt;

    // Raw acceleration (change of velocity per second)
    float raw_accel = (raw_velocity - prev_velocity) / dt;

    // EMA filter
    filtered_velocity = AXIS_FILTER_ALPHA * raw_velocity
                      + (1.0f - AXIS_FILTER_ALPHA) * filtered_velocity;
    filtered_accel = AXIS_FILTER_ALPHA * raw_accel
                   + (1.0f - AXIS_FILTER_ALPHA) * filtered_accel;

    // Save for next iteration
    prev_position = position;
    prev_velocity = raw_velocity;

    // Output as Q7.8 fixed-point
    *out_velocity_q8 = spi_float_to_velocity(filtered_velocity);
    *out_accel_q8 = spi_float_to_velocity(filtered_accel);
}
