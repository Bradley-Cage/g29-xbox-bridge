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
#define AUTH_BOOT_TIMEOUT_MS 15000  // 15s for auth controller to reach GIP_HOST_READY

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

    // Initialize TinyUSB device stack but IMMEDIATELY pull D+ low.
    // Xbox must NOT see us until auth controller is confirmed ready.
    // Risk: unanswered GIP 0x06 auth → Xbox error 0x82d60003 (permanent ban).
    tud_init(0);
    tud_disconnect();  // D+ low — invisible to Xbox
    printf("[XBOX] TinyUSB device initialized (D+ held LOW — Xbox gated)\n");

    // Initialize GIP protocol
    gip_init();
    printf("[GIP] Protocol initialized\n");

    // Initialize DMA SPI master
    spi_master_init();

    // Launch Core 1 (PIO-USB host + Equation VM)
    printf("[MAIN] Launching Core 1 (PIO-USB host + VM)...\n");
    multicore_launch_core1(core1_entry);

    // ── AUTH SAFETY GATE ──
    // Wait for PowerA auth controller to reach GIP_HOST_READY before
    // allowing Xbox to see this device. No watchdog during gate —
    // the timeout below is the safety net for Core 1 hangs.
    {
        printf("[SAFETY] Waiting for auth controller...\n");
        uint32_t gate_start = to_ms_since_boot(get_absolute_time());
        uint8_t last_state = 0xFF;

        while (true) {
            uint32_t elapsed = to_ms_since_boot(get_absolute_time()) - gate_start;

            // Log state transitions for diagnostics
            uint8_t state = auth_host_get_state();
            if (state != last_state) {
                static const char *state_names[] = {
                    "IDLE", "OPENING_EP", "WAIT_ANNOUNCE",
                    "WAIT_IDENTIFY", "POWERING_ON", "READY"
                };
                const char *name = (state <= 5) ? state_names[state] : "UNKNOWN";
                printf("[SAFETY] Auth state: %s (%lums)\n", name, (unsigned long)elapsed);
                last_state = state;
            }

            // Success: auth controller is GIP-ready
            if (auth_host_connected()) {
                printf("[SAFETY] Auth gate PASSED — controller ready (%lums)\n",
                       (unsigned long)elapsed);
                break;
            }

            // Timeout: refuse to connect to Xbox — device is safe-dead
            if (elapsed >= AUTH_BOOT_TIMEOUT_MS) {
                printf("\n[SAFETY] === AUTH CONTROLLER NOT READY AFTER %ds ===\n",
                       AUTH_BOOT_TIMEOUT_MS / 1000);
                printf("[SAFETY] Refusing Xbox connection to prevent error 0x82d60003.\n");
                printf("[SAFETY] Check: Is the Xbox Wireless controller plugged into Board X?\n");
                printf("[SAFETY] Device is safe-halted. Power cycle to retry.\n\n");
                // Stay here forever — D+ stays low, Xbox never sees us.
                // No watchdog enabled, so no automatic reset.
                while (true) {
                    sleep_ms(1000);
                }
            }

            sleep_ms(50);
        }
    }

    // Auth confirmed ready — NOW let Xbox see us
    tud_connect();  // D+ high — Xbox begins enumeration
    printf("[XBOX] D+ enabled — Xbox can now enumerate this device\n");

    // Enable watchdog AFTER both cores are fully operational
    watchdog_enable(WATCHDOG_TIMEOUT_MS, true);
    printf("[MAIN] Watchdog enabled (%dms)\n", WATCHDOG_TIMEOUT_MS);

    // Diagnostic: verify SPI GPIO functions weren't stolen by PIO-USB
    // func: 1=SPI, 5=SIO/GPIO, 6=PIO0, 7=PIO1
    {
        #include "hardware/structs/iobank0.h"
        for (int pin = 2; pin <= 5; pin++) {
            uint32_t ctrl = iobank0_hw->io[pin].ctrl;
            uint8_t func = ctrl & 0x1F;
            const char *name = (func == 1) ? "SPI" : (func == 6) ? "PIO0!" : (func == 7) ? "PIO1!" : "???";
            printf("[SPI-DIAG] GPIO%d func=%d (%s)\n", pin, func, name);
        }
    }

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

    // Safety: track auth controller presence for mid-session disconnect.
    // The PowerA controller transiently disconnects/reconnects after Xbox
    // enters CONFIG state (normal USB reset behavior). Use a debounce:
    // only trigger safety disconnect if auth stays gone for 3 seconds.
    bool auth_was_connected = true;
    uint32_t auth_lost_since = 0;  // 0 = not currently lost
    #define AUTH_DISCONNECT_DEBOUNCE_MS 3000

    while (true) {
        uint32_t now_ms = to_ms_since_boot(get_absolute_time());

        // ── TinyUSB device task (always, even if auth lost — handles USB teardown) ──
        tud_task();

        // ── SAFETY: Mid-session auth controller loss detection (#37) ──
        // Debounced: PowerA transiently disconnects after auth rounds complete.
        // Only trigger if it stays gone for 3 seconds.
        if (auth_was_connected) {
            if (!auth_host_connected()) {
                if (auth_lost_since == 0) {
                    auth_lost_since = now_ms;
                    printf("[SAFETY] Auth controller disconnected — watching (3s debounce)\n");
                } else if (now_ms - auth_lost_since >= AUTH_DISCONNECT_DEBOUNCE_MS) {
                    printf("\n[SAFETY] Auth controller GONE for %ds — disconnecting from Xbox\n",
                           AUTH_DISCONNECT_DEBOUNCE_MS / 1000);
                    printf("[SAFETY] Clean disconnect prevents error 0x82d60003.\n\n");
                    tud_disconnect();
                    auth_was_connected = false;
                }
            } else {
                if (auth_lost_since != 0) {
                    printf("[SAFETY] Auth controller reconnected (%lums glitch)\n",
                           (unsigned long)(now_ms - auth_lost_since));
                    auth_lost_since = 0;
                }
            }
        }

        // Skip all GIP/SPI/input if auth is lost — nothing useful to do
        if (!auth_was_connected) {
            sleep_ms(10);
            continue;
        }

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
                    // SAFETY (#38): disconnect rather than silently dropping
                    printf("\n[SAFETY] Auth relay timeout (5s) — disconnecting to prevent ban\n");
                    tud_disconnect();
                    auth_was_connected = false;
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

        // ── Periodic status (every 30s — quiet mode) ──
        if (now_ms - last_status_ms >= 30000) {
            printf("[STATUS] Xbox:%s SPI(rx=%lu err=%lu) Auth:%s Steer=%04X Gas=%02X Brake=%02X\n",
                   tud_mounted() ? "OK" : "--",
                   (unsigned long)spi_master_get_rx_count(),
                   (unsigned long)spi_master_get_error_count(),
                   gip_is_authenticated() ? "OK" : "--",
                   rx_frame.steering, rx_frame.gas, rx_frame.brake);
            last_status_ms = now_ms;
        }

        // Core 0 does NOT kick watchdog — only Core 1 does.
        // If Core 1 hangs, watchdog fires and resets both cores.
    }

    return 0;
}
