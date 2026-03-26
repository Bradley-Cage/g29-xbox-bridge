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
    uint8_t tx_seq = 0;
    bool spi_pending = false;

    // Auth relay buffer (local — no SPI fragmentation needed)
    uint8_t auth_buf[256];

    while (true) {
        uint32_t now_ms = to_ms_since_boot(get_absolute_time());

        // ── TinyUSB device task (always, handles Xbox USB events) ──
        tud_task();

        // ── GIP protocol task (keepalive, command processing) ──
        gip_task();

        // ── Auth: Forward challenges from Xbox → PowerA (local, no SPI) ──
        {
            uint16_t clen = gip_get_pending_auth(auth_buf, sizeof(auth_buf));
            if (clen > 0) {
                if (auth_host_send_challenge(auth_buf, clen)) {
                    printf("[AUTH] Challenge forwarded to PowerA (%d bytes)\n", clen);
                } else {
                    printf("[AUTH] ERROR: PowerA not ready for challenge\n");
                }
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

            spi_set_crc((uint8_t *)&tx_frame);
            spi_master_start_transfer(&tx_frame);
            spi_pending = true;
            last_spi_ms = now_ms;
        }

        // ── Periodic status ──
        if (now_ms - last_status_ms >= 5000) {
            printf("[STATUS] Xbox:%s SPI:%s Auth:%s(%s) Steer=%04X Gas=%02X Brake=%02X\n",
                   tud_mounted() ? "OK" : "--",
                   rx_frame.g29_status == 1 ? "OK" : "ERR",
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
