/**
 * main.c — RP2040 #1: Xbox GIP Device + Equation VM + SPI Master
 *
 * Core 0: TinyUSB device (Xbox), GIP protocol, SPI master
 * Core 1: Equation Language VM (dedicated, SRAM-resident)
 */

#include "pico/stdlib.h"
#include "pico/multicore.h"
#include "hardware/watchdog.h"
#include "hardware/spi.h"
#include "hardware/clocks.h"

#include "tusb.h"
#include "gip_protocol.h"
#include "gip_wheel.h"
#include "eq_vm.h"
#include "spi_protocol.h"

#include <stdio.h>
#include <string.h>

// ─── SPI Pin Assignments (Master) ───
#define SPI_MASTER_PORT     spi0
#define SPI_MASTER_SCK      18
#define SPI_MASTER_MOSI     19
#define SPI_MASTER_MISO     16
#define SPI_MASTER_CS       17

#define WATCHDOG_TIMEOUT_MS 100

// ─── Core 1: Equation VM ───

static void core1_entry(void) {
    // eq_vm_main_loop runs forever on Core 1
    eq_vm_init();
    eq_vm_main_loop();
}

// ─── Core 0: Main ───

int main(void) {
    // 200MHz certified clock (bump to 250 only if measured VM needs it)
    set_sys_clock_khz(200000, true);

    stdio_init_all();
    sleep_ms(500);

    printf("\n");
    printf("=== G29 Bridge — RP2040 #1 (Xbox + VM) ===\n");
    printf("Clock: %d MHz\n", clock_get_hz(clk_sys) / 1000000);
    printf("Build: %s %s\n\n", __DATE__, __TIME__);

    // Hardware watchdog
    watchdog_enable(WATCHDOG_TIMEOUT_MS, true);

    // Initialize TinyUSB device (Xbox connection)
    tud_init(0);
    printf("[XBOX] TinyUSB device initialized\n");

    // Initialize GIP protocol
    gip_init();
    printf("[GIP] Protocol initialized\n");

    // Initialize SPI master
    spi_init(SPI_MASTER_PORT, SPI_CLOCK_HZ);
    gpio_set_function(SPI_MASTER_SCK, GPIO_FUNC_SPI);
    gpio_set_function(SPI_MASTER_MOSI, GPIO_FUNC_SPI);
    gpio_set_function(SPI_MASTER_MISO, GPIO_FUNC_SPI);
    gpio_init(SPI_MASTER_CS);
    gpio_set_dir(SPI_MASTER_CS, GPIO_OUT);
    gpio_put(SPI_MASTER_CS, 1);  // CS high (inactive)
    printf("[SPI] Master initialized at %d MHz\n", SPI_CLOCK_HZ / 1000000);

    // Launch Core 1 (Equation VM)
    printf("[MAIN] Launching Core 1 (Equation VM)...\n");
    multicore_launch_core1(core1_entry);
    sleep_ms(50);

    printf("[MAIN] Entering main loop\n\n");

    // Frame buffers
    spi_upstream_frame_t rx_frame;
    spi_downstream_frame_t tx_frame;
    memset(&rx_frame, 0, sizeof(rx_frame));
    memset(&tx_frame, 0, sizeof(tx_frame));

    uint32_t last_spi_ms = 0;
    uint32_t last_status_ms = 0;
    uint8_t tx_seq = 0;

    while (true) {
        uint32_t now_ms = to_ms_since_boot(get_absolute_time());

        // ── TinyUSB device task ──
        tud_task();

        // ── GIP protocol task ──
        gip_task();

        // ── SPI exchange at 1kHz ──
        if (now_ms - last_spi_ms >= 1) {
            // Build downstream frame
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

            // SPI full-duplex exchange
            gpio_put(SPI_MASTER_CS, 0);
            spi_write_read_blocking(SPI_MASTER_PORT,
                                    (const uint8_t *)&tx_frame,
                                    (uint8_t *)&rx_frame,
                                    SPI_FRAME_SIZE);
            gpio_put(SPI_MASTER_CS, 1);

            // Validate received frame
            if (spi_validate_frame((const uint8_t *)&rx_frame)) {
                // Update VM axis state from G29 data (convert fixed-point to float)
                eq_vm_update_axes(rx_frame.steering,
                                  rx_frame.gas, rx_frame.brake, rx_frame.clutch,
                                  spi_velocity_to_float(rx_frame.steering_velocity),
                                  spi_velocity_to_float(rx_frame.steering_accel));
            }

            last_spi_ms = now_ms;
        }

        // ── Periodic status ──
        if (now_ms - last_status_ms >= 5000) {
            printf("[STATUS] Xbox:%s SPI_rx_ok:%s Steer=%04X\n",
                   tud_mounted() ? "OK" : "--",
                   spi_validate_frame((const uint8_t *)&rx_frame) ? "OK" : "ERR",
                   rx_frame.steering);
            last_status_ms = now_ms;
        }

        // ── Watchdog ──
        watchdog_update();
    }

    return 0;
}
