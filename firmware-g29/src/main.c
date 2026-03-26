/**
 * main.c — RP2040 #2: G29 Input + SPI Slave
 *
 * Core 0: PIO USB host (G29 only), SPI slave, G29 parsing, FFB output
 * Core 1: Secondary watchdog, diagnostics
 *
 * Auth relay has moved to Board X (RP2040 #1) — this board only handles
 * the G29 wheel and forwards input/FFB over SPI.
 */

#include "hardware/clocks.h"
#include "pico/stdlib.h"
#include "pico/multicore.h"
#include "hardware/watchdog.h"
#include "hardware/spi.h"

#include "bridge_config.h"
#include "g29_host.h"
#include "g29_parser.h"
#include "g29_ffb.h"
#include "spi_protocol.h"
#include "spi_slave.h"
#include "cdc_debug.h"
#include "led_status.h"

#include <stdio.h>
#include <string.h>

// External: axis_math velocity/acceleration computation
extern void axis_math_reset(void);
extern void axis_math_update(uint16_t raw_steering,
                              int16_t *out_velocity_q8,
                              int16_t *out_accel_q8);

// ─── Shared state ───

static volatile uint32_t core0_heartbeat = 0;
static spi_upstream_frame_t tx_frame;
static spi_downstream_frame_t rx_frame;

// ─── Core 1: Watchdog + Diagnostics ───

static void core1_entry(void) {
    uint32_t last_heartbeat = 0;
    uint32_t last_check = 0;

    while (true) {
        uint32_t now = to_ms_since_boot(get_absolute_time());

        // Monitor Core 0 heartbeat
        if (now - last_check >= 50) {
            uint32_t hb = core0_heartbeat;
            if (hb == last_heartbeat) {
                // Core 0 hasn't updated in 50ms — concerning but not fatal yet
                // Watchdog hardware will catch true lockups
            }
            last_heartbeat = hb;
            last_check = now;
        }

        // Kick watchdog from Core 1
        watchdog_update();
        sleep_ms(10);
    }
}

// ─── Core 0: Main Loop ───

int main(void) {
    // System clock must be multiple of 12MHz for USB timing
    set_sys_clock_khz(120000, true);

    // Initialize Pico SDK stdio (no drivers enabled — we add our own)
    stdio_init_all();

    // Initialize CDC debug output on native USB-C port
    cdc_debug_init();

    printf("\n");
    printf("=== G29 Bridge — RP2040 #2 (G29 + Auth) ===\n");
    printf("Build: %s %s\n\n", __DATE__, __TIME__);

    // Enable hardware watchdog (100ms)
    watchdog_enable(WATCHDOG_TIMEOUT_MS, true);

    // Launch Core 1
    multicore_launch_core1(core1_entry);

    // Initialize subsystems
    printf("[INIT] Status LED...\n");
    // led_status_init();  // DISABLED: testing if PIO1 conflicts with USB host

    printf("[INIT] G29 USB Host...\n");
    g29_host_init();

    printf("[INIT] SPI Slave...\n");
    spi_slave_init();

    printf("[INIT] Ready. Entering main loop.\n\n");

    uint32_t last_ffb_tick = 0;
    uint32_t last_status = 0;
    uint8_t tx_seq = 0;

    while (true) {
        uint32_t now_us = to_us_since_boot(get_absolute_time());
        uint32_t now_ms = now_us / 1000;

        // ── Step 1: Run PIO USB host task (G29 only) ──
        bool new_report = g29_host_task();

        // ── Step 2: Snapshot G29 state + update velocity on new report ──
        static g29_state_t gs;  // Local snapshot — not a pointer to shared state
        static int16_t vel_q8 = 0, accel_q8 = 0;
        if (new_report) {
            g29_snapshot_state(&gs);  // Atomic copy, no torn reads
            axis_math_update(gs.steering, &vel_q8, &accel_q8);
        }

        // ── Step 3: Build upstream SPI frame (input only, no auth) ──
        memset(&tx_frame, 0, sizeof(tx_frame));
        tx_frame.sequence = tx_seq++;
        tx_frame.g29_status = g29_get_status();
        tx_frame.frame_type = FRAME_UP_AXIS_STATE;
        tx_frame.steering = gs.steering;
        tx_frame.steering_velocity = vel_q8;
        tx_frame.steering_accel = accel_q8;
        tx_frame.gas = gs.gas;
        tx_frame.brake = gs.brake;
        tx_frame.clutch = gs.clutch;
        tx_frame.buttons0 = (gs.dpad & 0x0F)
                          | (gs.btn_cross ? 0x10 : 0)
                          | (gs.btn_square ? 0x20 : 0)
                          | (gs.btn_circle ? 0x40 : 0)
                          | (gs.btn_triangle ? 0x80 : 0);
        tx_frame.buttons1 = (gs.btn_r1 ? 0x01 : 0)
                          | (gs.btn_l1 ? 0x02 : 0)
                          | (gs.btn_r2 ? 0x04 : 0)
                          | (gs.btn_l2 ? 0x08 : 0)
                          | (gs.btn_share ? 0x10 : 0)
                          | (gs.btn_options ? 0x20 : 0)
                          | (gs.btn_r3 ? 0x40 : 0)
                          | (gs.btn_l3 ? 0x80 : 0);
        tx_frame.buttons2 = gs.shifter_gear | (gs.btn_plus ? 0x80 : 0);
        tx_frame.buttons3 = (gs.btn_minus ? 0x01 : 0)
                          | (gs.btn_dial_right ? 0x02 : 0)
                          | (gs.btn_dial_left ? 0x04 : 0)
                          | (gs.btn_enter ? 0x08 : 0)
                          | (gs.btn_ps ? 0x10 : 0);
        spi_set_crc((uint8_t *)&tx_frame);

        // ── Step 4: SPI exchange (slave responds to master clock) ──
        spi_slave_set_tx_frame(&tx_frame);
        bool spi_rx_ready = spi_slave_task();

        // ── Step 5: Process downstream commands (force output only) ──
        if (spi_rx_ready && spi_slave_get_rx_frame(&rx_frame) &&
            spi_validate_frame((const uint8_t *)&rx_frame)) {
            if (rx_frame.frame_type == FRAME_DN_FORCE_OUTPUT) {
                if (rx_frame.cmd_flags & CMD_FLAG_STOP_ALL_FFB) {
                    g29_ffb_stop_all();
                }
                g29_ffb_set_constant_force(spi_force_to_float(rx_frame.af_wheel));
            }
        }

        // ── Step 6: FFB tick (250Hz) ──
        if (now_ms - last_ffb_tick >= G29_FFB_MIN_INTERVAL_MS) {
            g29_ffb_tick();
            last_ffb_tick = now_ms;
        }

        // ── Step 7: Periodic status ──
        if (now_ms - last_status >= 5000) {
            printf("[STATUS] G29:%s SPI(rx=%lu err=%lu) Steer=%04X Gas=%02X Brake=%02X\n",
                   g29_is_connected() ? "OK" : "--",
                   spi_slave_get_rx_count(),
                   spi_slave_get_error_count(),
                   gs.steering, gs.gas, gs.brake);
            // Show last received downstream frame info
            if (spi_slave_get_rx_count() > 0) {
                printf("  [SPI-DN] type=%02X seq=%d force=%d pwr=%d angle=%d\n",
                       rx_frame.frame_type, rx_frame.sequence,
                       rx_frame.af_wheel, rx_frame.power_level,
                       rx_frame.angle_setting);
            }
            last_status = now_ms;
        }

        // ── Step 8: Update status LED (self-throttled to 250ms) ──
        if (g29_is_connected()) {
            // G29 connected — check SPI health
            if (spi_slave_get_error_count() > spi_slave_get_rx_count() / 2 &&
                spi_slave_get_rx_count() > 10) {
                led_status_set(LED_STATUS_SPI_ERROR);  // Yellow: SPI unreliable
            } else {
                led_status_set(LED_STATUS_OK);         // Green: all good
            }
        } else {
            led_status_set(LED_STATUS_WAITING);        // Blue: waiting for G29
        }
        led_status_update();

        // ── CDC debug flush ──
        cdc_debug_task();

        // ── Watchdog kick ──
        core0_heartbeat++;
        watchdog_update();
    }

    return 0;
}
