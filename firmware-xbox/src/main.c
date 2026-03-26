/**
 * main.c — RP2040 #1: Xbox GIP Device + PIO-USB Auth Host + Equation VM
 *
 * Core 0: TinyUSB device (Xbox GIP), GIP protocol, DMA SPI master, auth relay
 * Core 1: PIO-USB host (PowerA auth) + cooperative Equation VM ticks
 *
 * Clock: 144MHz (12x12, required for PIO-USB timing)
 */

#include "pico/stdlib.h"
#include "pico/multicore.h"
#include "hardware/watchdog.h"
#include "hardware/clocks.h"

#include "tusb.h"
#include "pio_usb.h"
#include "gip_protocol.h"
#include "gip_wheel.h"
#include "eq_vm.h"
#include "auth_host.h"
#include "spi_protocol.h"

#include <stdio.h>
#include <string.h>

#define WATCHDOG_TIMEOUT_MS 2000  // 2s during bring-up (tighten after stable)

// External: SPI master DMA driver (spi_master.c)
extern void spi_master_init(void);
extern void spi_master_start_transfer(const spi_downstream_frame_t *frame);
extern bool spi_master_transfer_done(void);
extern bool spi_master_finish_transfer(void);
extern bool spi_master_get_rx_frame(spi_upstream_frame_t *out);
extern uint32_t spi_master_get_rx_count(void);
extern uint32_t spi_master_get_error_count(void);

// ─── Core 1: PIO-USB Host + Cooperative Equation VM ───

static void core1_entry(void) {
    // Initialize PIO-USB host for PowerA controller
    auth_host_init();

    // Initialize Equation VM
    eq_vm_init();
    printf("[CORE1] PIO-USB host + Equation VM started\n");

    // Cooperative loop: service USB host + run VM ticks
    // IMPORTANT: Only Core 1 kicks watchdog — if Core 1 hangs (VM or USB),
    // watchdog fires. Core 0 hang is masked, but Core 0 is simple DMA/GIP.
    while (true) {
        // PIO-USB host task — fast return when idle, handles SOF
        auth_host_task();

        // One VM tick — must yield within 400us (57K cycles @ 144MHz)
        // to avoid missing PIO-USB SOF frames (1ms deadline)
        eq_vm_tick();

        // Sole watchdog kicker — detects Core 1 hangs
        watchdog_update();
    }
}

// ─── Core 0: Main ───

int main(void) {
    // 120MHz = 10x12MHz, required for PIO-USB timing
    set_sys_clock_khz(120000, true);

    stdio_init_all();
    sleep_ms(500);

    printf("\n");
    printf("=== G29 Bridge — RP2040 #1 (Xbox + VM) ===\n");
    printf("Clock: %lu MHz\n", clock_get_hz(clk_sys) / 1000000);
    printf("Build: %s %s\n\n", __DATE__, __TIME__);

    // Initialize TinyUSB device (Xbox connection)
    tud_init(0);
    printf("[XBOX] TinyUSB device initialized\n");

    // Initialize GIP protocol
    gip_init();
    printf("[GIP] Protocol initialized\n");

    // Initialize DMA SPI master
    spi_master_init();

    // Launch Core 1 (PIO-USB host + Equation VM)
    printf("[MAIN] Launching Core 1 (PIO-USB host + VM)...\n");
    multicore_launch_core1(core1_entry);
    sleep_ms(500);  // Give Core 1 time to init PIO-USB

    // Enable watchdog AFTER Core 1 is running
    watchdog_enable(WATCHDOG_TIMEOUT_MS, true);
    printf("[MAIN] Watchdog enabled (%dms)\n", WATCHDOG_TIMEOUT_MS);

    printf("[MAIN] Entering main loop\n\n");

    spi_upstream_frame_t rx_frame;
    spi_downstream_frame_t tx_frame;
    memset(&rx_frame, 0, sizeof(rx_frame));
    memset(&tx_frame, 0, sizeof(tx_frame));

    uint32_t last_spi_ms = 0;
    uint32_t last_status_ms = 0;
    uint32_t last_input_ms = 0;
    uint8_t tx_seq = 0;
    bool spi_pending = false;

    #define INPUT_REPORT_INTERVAL_MS 4  // ~250 Hz input reports to Xbox

    // Auth relay buffer (local — no SPI fragmentation needed)
    uint8_t auth_buf[256];
    uint16_t auth_pending_len = 0;  // Non-zero if challenge waiting for PowerA
    uint32_t auth_retry_start = 0;  // When we first tried to forward

    while (true) {
        uint32_t now_ms = to_ms_since_boot(get_absolute_time());

        // ── TinyUSB device task (always, handles Xbox USB events) ──
        tud_task();

        // ── GIP protocol task (keepalive, command processing) ──
        gip_task();

        // ── Auth: Forward challenges from Xbox → PowerA (with retry) ──
        {
            // Pick up new challenge from GIP if we don't already have one buffered
            if (auth_pending_len == 0) {
                uint16_t clen = gip_get_pending_auth(auth_buf, sizeof(auth_buf));
                if (clen > 0) {
                    auth_pending_len = clen;
                    auth_retry_start = now_ms;
                    printf("[AUTH] Challenge received from Xbox (%d bytes)\n", clen);
                }
            }

            // Try to forward buffered challenge to PowerA
            if (auth_pending_len > 0) {
                if (auth_host_send_challenge(auth_buf, auth_pending_len)) {
                    printf("[AUTH] Challenge forwarded to PowerA (%d bytes)\n", auth_pending_len);
                    auth_pending_len = 0;
                } else if (now_ms - auth_retry_start >= 5000) {
                    printf("[AUTH] ERROR: PowerA not ready after 5s, dropping challenge\n");
                    auth_pending_len = 0;
                }
                // Otherwise: retry next loop iteration
            }
        }

        // ── Auth: Check for response from PowerA → Xbox (local, no SPI) ──
        {
            uint16_t rlen = auth_host_get_response(auth_buf, sizeof(auth_buf));
            if (rlen > 0) {
                printf("[AUTH] Response from PowerA (%d bytes), sending to Xbox\n", rlen);
                gip_submit_auth_response(auth_buf, rlen);
            }
        }

        // ── SPI: Check if previous DMA transfer completed ──
        if (spi_pending && spi_master_transfer_done()) {
            if (spi_master_finish_transfer()) {
                spi_master_get_rx_frame(&rx_frame);
                eq_vm_update_axes(rx_frame.steering,
                                  rx_frame.gas, rx_frame.brake, rx_frame.clutch,
                                  spi_velocity_to_float(rx_frame.steering_velocity),
                                  spi_velocity_to_float(rx_frame.steering_accel));
            }
            spi_pending = false;
        }

        // ── Send wheel input report to Xbox at ~250Hz ──
        // TEMPORARY: bypass SPI gate, send hardcoded test data
        // to verify Xbox accepts our input report format.
        // TODO: restore g29_status check after SPI data path is fixed
        if (now_ms - last_input_ms >= INPUT_REPORT_INTERVAL_MS &&
            tud_vendor_mounted() && gip_is_authenticated()) {
            uint8_t report[8];
            uint8_t report_len;
            if (rx_frame.g29_status == G29_STATUS_CONNECTED) {
                // Real G29 data via SPI
                gip_wheel_build_state_report(&rx_frame, report, &report_len);
            } else {
                // Hardcoded test: centered wheel, pedals released
                memset(report, 0, 8);
                report[2] = 0xFF;  // Steering center low (0x7FFF)
                report[3] = 0x7F;  // Steering center high
                // Brake [4-5] = 0 (released), Gas [6-7] = 0 (released)
                report_len = 8;
            }
            gip_send_input_report(report, report_len);
            last_input_ms = now_ms;
        }

        // ── SPI: Start new transfer at 1kHz (input + force output only) ──
        if (!spi_pending && (now_ms - last_spi_ms >= 1)) {
            memset(&tx_frame, 0, sizeof(tx_frame));
            tx_frame.sequence = tx_seq++;

            // Force output from VM
            eq_force_output_t forces = eq_vm_get_forces();
            tx_frame.frame_type = FRAME_DN_FORCE_OUTPUT;
            tx_frame.af_wheel = spi_float_to_force(forces.af_wheel);
            tx_frame.af_motor_low = spi_float_to_force(forces.af_motor_low);
            tx_frame.af_motor_high = spi_float_to_force(forces.af_motor_high);
            tx_frame.power_level = 255;
            tx_frame.angle_setting = 900;

            // Encode GIP debug state in reserved[15..18]
            gip_debug_state_t dbg;
            gip_get_debug_state(&dbg);
            tx_frame.reserved[15] = dbg.flags;
            tx_frame.reserved[16] = dbg.announce_count;
            tx_frame.reserved[17] = dbg.auth_host;
            tx_frame.reserved[18] = dbg.last_rx_cmd;

            spi_set_crc((uint8_t *)&tx_frame);
            spi_master_start_transfer(&tx_frame);
            spi_pending = true;
            last_spi_ms = now_ms;
        }

        // ── Periodic status ──
        if (now_ms - last_status_ms >= 5000) {
            printf("[STATUS] Xbox:%s SPI(rx=%lu err=%lu pend=%d) Auth:%s(%s) "
                   "Steer=%04X Gas=%02X Brake=%02X\n",
                   tud_mounted() ? "OK" : "--",
                   (unsigned long)spi_master_get_rx_count(),
                   (unsigned long)spi_master_get_error_count(),
                   spi_pending ? 1 : 0,
                   gip_is_authenticated() ? "OK" : "--",
                   auth_host_connected() ? "conn" : "wait",
                   rx_frame.steering, rx_frame.gas, rx_frame.brake);
            last_status_ms = now_ms;
        }

        // Core 0 does NOT kick watchdog — only Core 1 does.
        // If Core 1 hangs, watchdog fires and resets both cores.
    }

    return 0;
}
