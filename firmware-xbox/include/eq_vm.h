/**
 * eq_vm.h — Xbox Equation Language Virtual Machine
 */
#ifndef EQ_VM_H
#define EQ_VM_H

#include <stdint.h>
#include <stdbool.h>

#define EQ_MAX_EQUATIONS    16
#define EQ_BYTECODE_POOL    2048
#define EQ_APP_MEMORY_SIZE  512
#define EQ_INSTRUCTION_LIMIT 200

typedef enum {
    EQ_UNSET = 0,
    EQ_RUNNING,
    EQ_STOPPED,
    EQ_PAUSED,
    EQ_FAULTED
} eq_state_t;

typedef struct {
    float af_wheel;         // Axis 0: wheel torque (-1.0 to 1.0)
    float af_motor_low;     // Axis 5: low freq vibration
    float af_motor_high;    // Axis 6: high freq vibration
} eq_force_output_t;

void eq_vm_init(void);
void eq_vm_main_loop(void);     // Runs on Core 1 — never returns

// Commands from GIP protocol (called from Core 0 via queue)
void eq_vm_set_equation(uint8_t index, const uint8_t *bytecode,
                        uint16_t size, uint16_t offset);
void eq_vm_set_state(uint8_t index, eq_state_t state);
void eq_vm_set_app_memory(uint16_t addr, uint32_t value);
void eq_vm_set_all_states(uint8_t loop_state, const uint8_t *eq_states);

// Read latest force output (called from Core 0)
eq_force_output_t eq_vm_get_forces(void);

// Update axis state from G29 data (called from Core 0)
void eq_vm_update_axes(uint16_t steering, uint8_t gas, uint8_t brake,
                       uint8_t clutch, float velocity, float accel);

#endif
