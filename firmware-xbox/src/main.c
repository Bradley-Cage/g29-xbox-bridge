/**
 * main.c — RP2040 #1: Xbox GIP Device + Equation VM + SPI Master
 *
 * Core 0: TinyUSB device (Xbox), GIP protocol, DMA SPI master
 * Core 1: Equation Language VM (dedicated, SRAM-resident)
 */

#include "pico/stdlib.h"
#include "pico/multicore.h"
#include "hardware/watchdog.h"
#include "hardware/clocks.h"

#include "tusb.h"
#include "gip_protocol.h"
#include "gip_wheel.h"
#include "eq_vm.h"
#include "spi_protocol.h"

#include <stdio.h>
#include <string.h>

#define WATCHDOG_TIMEOUT_MS 100

// External: SPI master DMA driver (spi_master.c)
extern void spi_master_init(void);
extern void spi_master_start_transfer(const spi_downstream_frame_t *frame);
extern bool spi_master_transfer_done(void);
extern bool spi_master_finish_transfer(void);
extern bool spi_master_get_rx_frame(spi_upstream_frame_t *out);

// ─── Core 1: Equation VM ───

static void core1_entry(void) {
    eq_vm_init();
    eq_vm_main_loop();
}

// ─── Core 0: Main ───

int main(void) {
    // 200MHz certified (bump to 250 only if measured VM needs it)
    set_sys_clock_khz(200000, true);

    stdio_init_all();
    sleep_ms(500);

    printf("\n");
    printf("=== G29 Bridge — RP2040 #1 (Xbox + VM) ===\n");
    printf("Clock: %lu MHz\n", clock_get_hz(clk_sys) / 1000000);
    printf("Build: %s %s\n\n", __DATE__, __TIME__);

    // Hardware watchdog
    watchdog_enable(WATCHDOG_TIMEOUT_MS, true);

    // Initialize TinyUSB device (Xbox connection)
    tud_init(0);
    printf("[XBOX] TinyUSB device initialized\n");

    // Initialize GIP protocol
    gip_init();
    printf("[GIP] Protocol initialized\n");

    // Initialize DMA SPI master
    spi_master_init();

    // Launch Core 1 (Equation VM)
    printf("[MAIN] Launching Core 1 (Equation VM)...\n");
    multicore_launch_core1(core1_entry);
    sleep_ms(50);

    printf("[MAIN] Entering main loop\n\n");

    spi_upstream_frame_t rx_frame;
    spi_downstream_frame_t tx_frame;
    memset(&rx_frame, 0, sizeof(rx_frame));
    memset(&tx_frame, 0, sizeof(tx_frame));

    uint32_t last_spi_ms = 0;
    uint32_t last_status_ms = 0;
    uint8_t tx_seq = 0;
    bool spi_pending = false;

    while (true) {
        uint32_t now_ms = to_ms_since_boot(get_absolute_time());

        // ── TinyUSB device task (always, handles Xbox USB events) ──
        tud_task();

        // ── GIP protocol task (keepalive, command processing) ──
        gip_task();

        // ── SPI: Check if previous DMA transfer completed ──
        if (spi_pending && spi_master_transfer_done()) {
            if (spi_master_finish_transfer()) {
                // Got a valid frame — update VM axis state
                spi_master_get_rx_frame(&rx_frame);
                eq_vm_update_axes(rx_frame.steering,
                                  rx_frame.gas, rx_frame.brake, rx_frame.clutch,
                                  spi_velocity_to_float(rx_frame.steering_velocity),
                                  spi_velocity_to_float(rx_frame.steering_accel));
            }
            spi_pending = false;
        }

        // ── SPI: Start new transfer at 1kHz ──
        if (!spi_pending && (now_ms - last_spi_ms >= 1)) {
            eq_force_output_t forces = eq_vm_get_forces();
            memset(&tx_frame, 0, sizeof(tx_frame));
            tx_frame.frame_type = FRAME_DN_FORCE_OUTPUT;
            tx_frame.sequence = tx_seq++;
            tx_frame.af_wheel = spi_float_to_force(forces.af_wheel);
            tx_frame.af_motor_low = spi_float_to_force(forces.af_motor_low);
            tx_frame.af_motor_high = spi_float_to_force(forces.af_motor_high);
            tx_frame.power_level = 255;
            tx_frame.angle_setting = 900;
            spi_set_crc((uint8_t *)&tx_frame);

            // Kick off DMA — non-blocking, returns immediately
            spi_master_start_transfer(&tx_frame);
            spi_pending = true;
            last_spi_ms = now_ms;
        }

        // ── Periodic status ──
        if (now_ms - last_status_ms >= 5000) {
            printf("[STATUS] Xbox:%s SPI:%s Steer=%04X Gas=%02X Brake=%02X\n",
                   tud_mounted() ? "OK" : "--",
                   rx_frame.g29_status == 1 ? "OK" : "ERR",
                   rx_frame.steering, rx_frame.gas, rx_frame.brake);
            last_status_ms = now_ms;
        }

        // ── Watchdog ──
        watchdog_update();
    }

    return 0;
}
