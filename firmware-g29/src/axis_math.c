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

#include <stdint.h>

// State
static float prev_position = 0.0f;      // Previous normalized position
static float prev_velocity = 0.0f;      // Previous velocity for accel calc
static float filtered_velocity = 0.0f;  // EMA-filtered velocity
static float filtered_accel = 0.0f;     // EMA-filtered acceleration
static bool first_sample = true;

// EMA filter coefficient (0.0-1.0, lower = smoother, higher = more responsive)
#define AXIS_FILTER_ALPHA   0.15f

// Sample period in seconds (1kHz)
#define AXIS_DT             0.001f

void axis_math_reset(void) {
    prev_position = 0.0f;
    prev_velocity = 0.0f;
    filtered_velocity = 0.0f;
    filtered_accel = 0.0f;
    first_sample = true;
}

void axis_math_update(uint16_t raw_steering,
                      int16_t *out_velocity_q8,
                      int16_t *out_accel_q8) {
    // Normalize steering: 0x0000(-1.0) → 0x8000(0.0) → 0xFFFF(+1.0)
    float position = ((float)raw_steering / 32768.0f) - 1.0f;

    if (first_sample) {
        prev_position = position;
        prev_velocity = 0.0f;
        first_sample = false;
        *out_velocity_q8 = 0;
        *out_accel_q8 = 0;
        return;
    }

    // Raw velocity (change per second)
    float raw_velocity = (position - prev_position) / AXIS_DT;

    // Raw acceleration (change of velocity per second)
    float raw_accel = (raw_velocity - prev_velocity) / AXIS_DT;

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
