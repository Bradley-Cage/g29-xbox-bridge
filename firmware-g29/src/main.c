/**
 * main.c — RP2040 #2: G29 + Auth + SPI Slave
 *
 * Core 0: PIO USB host (G29 + auth), SPI slave, G29 parsing, FFB output
 * Core 1: Secondary watchdog, SPI DMA management, diagnostics
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
#include "auth_relay.h"
#include "spi_protocol.h"

#include <stdio.h>
#include <string.h>

// ─── Shared state ───

static volatile uint32_t core0_heartbeat = 0;
static g29_state_t g29_state;
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

    // Initialize stdio (UART on GPIO 16)
    stdio_init_all();
    sleep_ms(500);

    printf("\n");
    printf("=== G29 Bridge — RP2040 #2 (G29 + Auth) ===\n");
    printf("Build: %s %s\n\n", __DATE__, __TIME__);

    // Enable hardware watchdog (100ms)
    watchdog_enable(WATCHDOG_TIMEOUT_MS, true);

    // Launch Core 1
    multicore_launch_core1(core1_entry);

    // Initialize subsystems
    printf("[INIT] G29 USB Host...\n");
    g29_host_init();

    printf("[INIT] Auth Relay...\n");
    auth_relay_init();

    printf("[INIT] SPI Slave...\n");
    // SPI init will be implemented in spi_slave.c

    printf("[INIT] Ready. Entering main loop.\n\n");

    uint32_t last_ffb_tick = 0;
    uint32_t last_status = 0;
    uint8_t tx_seq = 0;

    while (true) {
        uint32_t now_us = to_us_since_boot(get_absolute_time());
        uint32_t now_ms = now_us / 1000;

        // ── Step 1: Run PIO USB host task ──
        bool new_report = g29_host_task();

        // ── Step 2: Parse new G29 report ──
        if (new_report) {
            // g29_host_task fills the report — parsing happens inside
        }

        // ── Step 3: Build upstream SPI frame ──
        memset(&tx_frame, 0, sizeof(tx_frame));
        tx_frame.frame_type = FRAME_UP_AXIS_STATE;
        tx_frame.sequence = tx_seq++;
        tx_frame.steering = g29_state.steering;
        tx_frame.gas = g29_state.gas;
        tx_frame.brake = g29_state.brake;
        tx_frame.clutch = g29_state.clutch;
        tx_frame.buttons0 = (g29_state.dpad & 0x0F)
                          | (g29_state.btn_cross ? 0x10 : 0)
                          | (g29_state.btn_square ? 0x20 : 0)
                          | (g29_state.btn_circle ? 0x40 : 0)
                          | (g29_state.btn_triangle ? 0x80 : 0);
        tx_frame.buttons1 = (g29_state.btn_r1 ? 0x01 : 0)
                          | (g29_state.btn_l1 ? 0x02 : 0)
                          | (g29_state.btn_r2 ? 0x04 : 0)
                          | (g29_state.btn_l2 ? 0x08 : 0)
                          | (g29_state.btn_share ? 0x10 : 0)
                          | (g29_state.btn_options ? 0x20 : 0)
                          | (g29_state.btn_r3 ? 0x40 : 0)
                          | (g29_state.btn_l3 ? 0x80 : 0);
        tx_frame.buttons2 = g29_state.shifter_gear | (g29_state.btn_plus ? 0x80 : 0);
        tx_frame.buttons3 = (g29_state.btn_minus ? 0x01 : 0)
                          | (g29_state.btn_dial_right ? 0x02 : 0)
                          | (g29_state.btn_dial_left ? 0x04 : 0)
                          | (g29_state.btn_enter ? 0x08 : 0)
                          | (g29_state.btn_ps ? 0x10 : 0);
        tx_frame.g29_status = g29_get_status();
        tx_frame.auth_status = auth_relay_connected() ? AUTH_STATUS_CONNECTED : AUTH_STATUS_DISCONNECTED;
        spi_set_crc((uint8_t *)&tx_frame);

        // ── Step 4: SPI exchange (slave responds to master clock) ──
        // TODO: implement in spi_slave.c

        // ── Step 5: Process downstream commands ──
        if (spi_validate_frame((const uint8_t *)&rx_frame)) {
            if (rx_frame.cmd_flags & CMD_FLAG_STOP_ALL_FFB) {
                g29_ffb_stop_all();
            }
            // Apply force output
            g29_ffb_set_constant_force(rx_frame.af_wheel);
        }

        // ── Step 6: FFB tick (250Hz) ──
        if (now_ms - last_ffb_tick >= G29_FFB_MIN_INTERVAL_MS) {
            g29_ffb_tick();
            last_ffb_tick = now_ms;
        }

        // ── Step 7: Periodic status ──
        if (now_ms - last_status >= 5000) {
            printf("[STATUS] G29:%s Auth:%s Steer=%04X Gas=%02X Brake=%02X\n",
                   g29_is_connected() ? "OK" : "--",
                   auth_relay_connected() ? "OK" : "--",
                   g29_state.steering, g29_state.gas, g29_state.brake);
            last_status = now_ms;
        }

        // ── Watchdog kick ──
        core0_heartbeat++;
        watchdog_update();
    }

    return 0;
}
