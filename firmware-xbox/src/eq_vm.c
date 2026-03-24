/**
 * eq_vm.c — Equation Language VM (Stub)
 */
#include "eq_vm.h"
#include "pico/stdlib.h"
#include "hardware/watchdog.h"
#include <string.h>
#include <stdio.h>

static eq_force_output_t current_forces;
static volatile bool axes_updated = false;

void eq_vm_init(void) {
    memset(&current_forces, 0, sizeof(current_forces));
    printf("[VM] Equation Language VM initialized\n");
}

void __attribute__((section(".time_critical"))) eq_vm_main_loop(void) {
    printf("[VM] Core 1 main loop started\n");

    while (true) {
        // TODO: Phase 5 — implement equation execution loop
        watchdog_update();
        sleep_ms(1);  // Placeholder 1kHz tick
    }
}

void eq_vm_set_equation(uint8_t index, const uint8_t *bytecode,
                        uint16_t size, uint16_t offset) {
    (void)index; (void)bytecode; (void)size; (void)offset;
    // TODO: Phase 5
}

void eq_vm_set_state(uint8_t index, eq_state_t state) {
    (void)index; (void)state;
    // TODO: Phase 5
}

void eq_vm_set_app_memory(uint16_t addr, uint32_t value) {
    (void)addr; (void)value;
    // TODO: Phase 5
}

void eq_vm_set_all_states(uint8_t loop_state, const uint8_t *eq_states) {
    (void)loop_state; (void)eq_states;
    // TODO: Phase 5
}

eq_force_output_t eq_vm_get_forces(void) {
    return current_forces;
}

void eq_vm_update_axes(uint16_t steering, uint8_t gas, uint8_t brake,
                       uint8_t clutch, float velocity, float accel) {
    (void)steering; (void)gas; (void)brake; (void)clutch;
    (void)velocity; (void)accel;
    // TODO: Phase 5 — write to double-buffered axis state
}
