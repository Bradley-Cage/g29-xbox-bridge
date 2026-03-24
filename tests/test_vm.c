/**
 * test_vm.c — Native macOS test harness for the Equation Language VM
 *
 * Compiles and runs natively (not cross-compiled for ARM).
 * Tests opcode implementations against spec examples from H001443.
 *
 * Build:   cc -o test_vm test_vm.c -lm -I../firmware-xbox/include -DTEST_NATIVE
 * Run:     ./test_vm
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <assert.h>

// ─── Minimal type stubs for native compilation ───
#ifndef TEST_NATIVE
#define TEST_NATIVE
#endif

typedef unsigned char uint8_t;
typedef unsigned short uint16_t;
typedef unsigned int uint32_t;
typedef signed short int16_t;
typedef signed int int32_t;
typedef unsigned long long uint64_t;
typedef int bool;
#define true 1
#define false 0

// ─── Pull in the opcode types (redefine eq_state_t for native) ───

typedef enum {
    EQ_UNSET = 0,
    EQ_RUNNING,
    EQ_STOPPED,
    EQ_PAUSED,
    EQ_FAULTED
} eq_state_t;

// ─── VM state (mirrors eq_opcodes.c internal structure) ───

typedef struct {
    float registers[32];  // L0-L7, E0-E3, timing, axis, etc.
    float app_memory[512];
    uint8_t bytecode_pool[2048];
    struct {
        eq_state_t state;
        uint16_t size;
        uint16_t offset;
        uint16_t pc;
        float E[4];
    } equations[16];

    struct {
        float displacement;
        float velocity;
        float acceleration;
        float displacement_rad;
        float force;
        float prev_force;
    } axes[8];

    float loop_period;
    float loop_avg_period;
    uint32_t loop_count;
    uint32_t loop_time_100us;

    uint8_t current_eq;
    uint8_t current_axis;
    uint32_t instructions_executed;
} vm_state_t;

// ─── Include the trig table init and dispatch (reimplemented for native) ───

#define TRIG_TABLE_SIZE 256
static float sin_table[TRIG_TABLE_SIZE + 1];

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

static void init_trig_table(void) {
    for (int i = 0; i <= TRIG_TABLE_SIZE; i++) {
        float angle = ((float)i / TRIG_TABLE_SIZE) * (M_PI / 2.0f);
        sin_table[i] = sinf(angle);
    }
}

static float fast_sinf(float x) {
    const float TWO_PI = 2.0f * M_PI;
    x = x - TWO_PI * (float)(int)(x / TWO_PI);
    if (x < 0.0f) x += TWO_PI;

    int quadrant;
    float normalized;
    if (x < M_PI / 2.0f) { quadrant = 0; normalized = x; }
    else if (x < M_PI) { quadrant = 1; normalized = M_PI - x; }
    else if (x < 3.0f * M_PI / 2.0f) { quadrant = 2; normalized = x - M_PI; }
    else { quadrant = 3; normalized = TWO_PI - x; }

    float idx_f = normalized * (2.0f * TRIG_TABLE_SIZE / M_PI);
    int idx = (int)idx_f;
    if (idx >= TRIG_TABLE_SIZE) idx = TRIG_TABLE_SIZE - 1;
    float frac = idx_f - (float)idx;
    float result = sin_table[idx] + frac * (sin_table[idx + 1] - sin_table[idx]);
    if (quadrant >= 2) result = -result;
    return result;
}

// ─── Operand decode (simplified for test — handles the operands used in spec examples) ───

static float decode_operand_test(vm_state_t *vm, uint8_t operand,
                                  uint16_t *pc, const uint8_t *code, uint16_t code_size,
                                  bool *is_writable, float **write_target) {
    *is_writable = false;
    *write_target = NULL;
    uint8_t eq = vm->current_eq;
    uint8_t axis = vm->current_axis;

    switch (operand) {
        case 0x00: return 0.0f;
        case 0x01: case 0x02: case 0x03: case 0x04:
        case 0x05: case 0x06: case 0x07:
            return (float)operand;

        case 0x10: { // 1-byte immediate
            uint8_t val = code[(*pc)++];
            return (float)val;
        }

        case 0x13: { // 4-byte float (big-endian)
            uint32_t raw = ((uint32_t)code[*pc] << 24) | ((uint32_t)code[*pc+1] << 16)
                         | ((uint32_t)code[*pc+2] << 8) | code[*pc+3];
            *pc += 4;
            float result;
            memcpy(&result, &raw, 4);
            return result;
        }

        case 0x19: { // signed byte / 100
            int8_t val = (int8_t)code[(*pc)++];
            return (float)val / 100.0f;
        }

        // E0-E3
        case 0x30: *is_writable = true; *write_target = &vm->equations[eq].E[0]; return vm->equations[eq].E[0];
        case 0x31: *is_writable = true; *write_target = &vm->equations[eq].E[1]; return vm->equations[eq].E[1];
        case 0x32: *is_writable = true; *write_target = &vm->equations[eq].E[2]; return vm->equations[eq].E[2];
        case 0x33: *is_writable = true; *write_target = &vm->equations[eq].E[3]; return vm->equations[eq].E[3];

        // Et (equation time, 100us ticks)
        case 0x3B: return (float)vm->loop_time_100us;

        // As (axis selector)
        case 0x50: *is_writable = true; *write_target = (float*)&vm->current_axis; return (float)vm->current_axis;

        // Ap (prev force)
        case 0x51: return vm->axes[axis].prev_force;

        // Af (force output)
        case 0x52: *is_writable = true; *write_target = &vm->axes[axis].force; return vm->axes[axis].force;

        // Af+ (accumulate)
        case 0x53: *is_writable = true; *write_target = &vm->axes[axis].force; return vm->axes[axis].force;

        // Ad (displacement)
        case 0x54: return vm->axes[axis].displacement;

        default:
            printf("  [WARN] Unknown operand 0x%02X\n", operand);
            return 0.0f;
    }
}

// ─── Opcode execution (simplified for test) ───

static bool exec_opcode(vm_state_t *vm, uint8_t opcode,
                         uint16_t *pc, const uint8_t *code, uint16_t code_size) {
    float in1, in2, out;
    bool w1, w2, w3;
    float *t1, *t2, *t3;

    switch (opcode) {
        case 0x00: return true; // NOP
        case 0x04: *pc = code_size; return true; // END

        case 0x12: { // MUL — decode operands sequentially (each may consume immediate bytes)
            uint8_t op1 = code[(*pc)++];
            in1 = decode_operand_test(vm, op1, pc, code, code_size, &w1, &t1);
            uint8_t op2 = code[(*pc)++];
            in2 = decode_operand_test(vm, op2, pc, code, code_size, &w2, &t2);
            uint8_t op3 = code[(*pc)++];
            decode_operand_test(vm, op3, pc, code, code_size, &w3, &t3);
            out = in1 * in2;
            if (w3 && t3) {
                if (op3 == 0x53) *t3 += out;
                else *t3 = out;
            }
            return true;
        }

        case 0x10: { // ADD
            uint8_t op1 = code[(*pc)++];
            in1 = decode_operand_test(vm, op1, pc, code, code_size, &w1, &t1);
            uint8_t op2 = code[(*pc)++];
            in2 = decode_operand_test(vm, op2, pc, code, code_size, &w2, &t2);
            uint8_t op3 = code[(*pc)++];
            decode_operand_test(vm, op3, pc, code, code_size, &w3, &t3);
            out = in1 + in2;
            if (w3 && t3) { if (op3 == 0x53) *t3 += out; else *t3 = out; }
            return true;
        }

        case 0x11: { // SUB
            uint8_t op1 = code[(*pc)++];
            in1 = decode_operand_test(vm, op1, pc, code, code_size, &w1, &t1);
            uint8_t op2 = code[(*pc)++];
            in2 = decode_operand_test(vm, op2, pc, code, code_size, &w2, &t2);
            uint8_t op3 = code[(*pc)++];
            decode_operand_test(vm, op3, pc, code, code_size, &w3, &t3);
            out = in1 - in2;
            if (w3 && t3) { if (op3 == 0x53) *t3 += out; else *t3 = out; }
            return true;
        }

        case 0xA0: { // MOV
            uint8_t op1 = code[(*pc)++], op2 = code[(*pc)++];
            in1 = decode_operand_test(vm, op1, pc, code, code_size, &w1, &t1);
            decode_operand_test(vm, op2, pc, code, code_size, &w2, &t2);
            if (w2 && t2) {
                if (op2 == 0x50) vm->current_axis = (uint8_t)(int)in1;
                else if (op2 == 0x53) *t2 += in1;
                else *t2 = in1;
            }
            return true;
        }

        default:
            printf("  [ERR] Unhandled opcode 0x%02X\n", opcode);
            return false;
    }
}

// ─── Run one equation ───

static bool run_equation(vm_state_t *vm, int eq_idx, const uint8_t *code, uint16_t size) {
    vm->current_eq = eq_idx;
    vm->current_axis = 0;
    vm->axes[0].force = 0.0f; // Reset Af at start of loop

    uint16_t pc = 0;
    int ic = 0;
    while (pc < size && ic < 200) {
        uint8_t opcode = code[pc++];
        if (!exec_opcode(vm, opcode, &pc, code, size)) {
            printf("  FAULT at instruction %d (opcode 0x%02X)\n", ic, opcode);
            return false;
        }
        ic++;
    }
    return true;
}

// ─── Test Infrastructure ───

static int tests_passed = 0;
static int tests_failed = 0;

#define ASSERT_NEAR(actual, expected, tolerance, msg) do { \
    float _a = (actual), _e = (expected), _t = (tolerance); \
    if (fabsf(_a - _e) > _t) { \
        printf("  FAIL: %s — expected %.6f, got %.6f (delta %.6f > %.6f)\n", \
               msg, _e, _a, fabsf(_a - _e), _t); \
        tests_failed++; \
    } else { \
        printf("  PASS: %s — %.6f (expected %.6f)\n", msg, _a, _e); \
        tests_passed++; \
    } \
} while(0)

// ─── Test Cases from H001443 Spec ───

// Test 1: Simple Hooks Law Spring — f(x) = -kx, k = 0.5
// Equation: MOV 0 As; MUL Ad -0.5f Af+
// Assembled: A0 00 50 12 54 13 BF 00 00 00 53
static void test_simple_spring(void) {
    printf("\n--- Test: Simple Hooks Law Spring (f=-0.5*x) ---\n");

    uint8_t code[] = {
        0xA0, 0x00, 0x50,                              // MOV 0 As (select wheel axis)
        0x12, 0x54, 0x13, 0xBF, 0x00, 0x00, 0x00, 0x53 // MUL Ad -0.5f Af+
    };

    vm_state_t vm;
    memset(&vm, 0, sizeof(vm));

    // Test at various wheel positions
    float test_positions[] = {0.0f, 0.5f, -0.5f, 1.0f, -1.0f, 0.1f};
    for (int i = 0; i < 6; i++) {
        memset(&vm, 0, sizeof(vm));
        vm.axes[0].displacement = test_positions[i];

        run_equation(&vm, 0, code, sizeof(code));

        float expected = -0.5f * test_positions[i];
        char msg[64];
        snprintf(msg, sizeof(msg), "Spring at Ad=%.2f", test_positions[i]);
        ASSERT_NEAR(vm.axes[0].force, expected, 0.001f, msg);
    }
}

// Test 2: Spring with offset — f(x) = -k(x - x0), k=0.5, x0=0.1
// Assembled: SUB Ad 0.1f E0; MUL E0 -0.5f Af+
static void test_spring_with_offset(void) {
    printf("\n--- Test: Spring with Offset (f=-0.5*(x-0.1)) ---\n");

    // 0x3DCCCCCD = 0.1f in IEEE 754
    // 0xBF000000 = -0.5f in IEEE 754
    uint8_t code[] = {
        0x11, 0x54, 0x13, 0x3D, 0xCC, 0xCC, 0xCD, 0x30, // SUB Ad 0.1f E0
        0x12, 0x30, 0x13, 0xBF, 0x00, 0x00, 0x00, 0x53   // MUL E0 -0.5f Af+
    };

    vm_state_t vm;
    memset(&vm, 0, sizeof(vm));
    vm.axes[0].displacement = 0.5f;

    run_equation(&vm, 0, code, sizeof(code));

    float expected = -0.5f * (0.5f - 0.1f);
    ASSERT_NEAR(vm.axes[0].force, expected, 0.001f, "Spring with offset at Ad=0.5");
}

// Test 3: Multi-axis — throttle-proportional spring
// MOV 1 AS; SUB 0 AD E0; MOV 0 AS; POS AD E1; ENDZ E1; MUL E0 AD AF+
static void test_multi_axis(void) {
    printf("\n--- Test: Multi-axis (brake-proportional force) ---\n");

    // Simplified version: MOV 1 As; MOV Ad E0; MOV 0 As; MUL E0 Ad Af+
    uint8_t code[] = {
        0xA0, 0x01, 0x50,       // MOV 1 As (select throttle axis)
        0xA0, 0x54, 0x30,       // MOV Ad E0 (E0 = throttle displacement)
        0xA0, 0x00, 0x50,       // MOV 0 As (select wheel axis)
        0x12, 0x30, 0x54, 0x53  // MUL E0 Ad Af+ (force = throttle * wheel_pos)
    };

    vm_state_t vm;
    memset(&vm, 0, sizeof(vm));
    vm.axes[0].displacement = 0.3f;  // Wheel at 30% right
    vm.axes[1].displacement = 0.8f;  // Throttle at 80%

    run_equation(&vm, 0, code, sizeof(code));

    float expected = 0.8f * 0.3f;  // throttle * wheel
    ASSERT_NEAR(vm.axes[0].force, expected, 0.001f, "Multi-axis force");
}

// Test 4: Trig accuracy
static void test_trig_accuracy(void) {
    printf("\n--- Test: Trig LUT Accuracy ---\n");

    float test_angles[] = {0.0f, M_PI/6, M_PI/4, M_PI/3, M_PI/2,
                           M_PI, 3*M_PI/2, 2*M_PI, -M_PI/4, 5.5f};

    for (int i = 0; i < 10; i++) {
        float angle = test_angles[i];
        float lut_sin = fast_sinf(angle);
        float ref_sin = sinf(angle);
        float error = fabsf(lut_sin - ref_sin);

        char msg[64];
        snprintf(msg, sizeof(msg), "sin(%.4f) error", angle);
        ASSERT_NEAR(lut_sin, ref_sin, 0.005f, msg); // 0.5% tolerance
    }
}

// Test 5: Force accumulation (Af+ semantics)
static void test_af_accumulate(void) {
    printf("\n--- Test: Af+ Accumulation ---\n");

    // Two equations each adding to Af+
    // Eq 0: MUL Ad -0.3f Af+ (spring component)
    // Eq 1: MUL Ad -0.2f Af+ (damper component)
    // Total should be Af = (-0.3 + -0.2) * Ad = -0.5 * Ad

    uint8_t code0[] = {
        0xA0, 0x00, 0x50,
        0x12, 0x54, 0x13, 0xBE, 0x99, 0x99, 0x9A, 0x53  // MUL Ad -0.3f Af+
    };
    uint8_t code1[] = {
        0xA0, 0x00, 0x50,
        0x12, 0x54, 0x13, 0xBE, 0x4C, 0xCC, 0xCD, 0x53  // MUL Ad -0.2f Af+
    };

    vm_state_t vm;
    memset(&vm, 0, sizeof(vm));
    vm.axes[0].displacement = 0.5f;

    // Run equation 0
    run_equation(&vm, 0, code0, sizeof(code0));
    float after_eq0 = vm.axes[0].force;

    // DON'T reset Af — the loop accumulates across equations
    // Run equation 1 (without resetting Af)
    vm.current_eq = 1;
    vm.current_axis = 0;
    uint16_t pc = 0;
    int ic = 0;
    while (pc < sizeof(code1) && ic < 200) {
        uint8_t opcode = code1[pc++];
        exec_opcode(&vm, opcode, &pc, code1, sizeof(code1));
        ic++;
    }

    float expected_eq0 = -0.3f * 0.5f;
    float expected_total = (-0.3f + -0.2f) * 0.5f;

    ASSERT_NEAR(after_eq0, expected_eq0, 0.001f, "Eq0 Af contribution");
    ASSERT_NEAR(vm.axes[0].force, expected_total, 0.001f, "Accumulated Af total");
}

// Test 6: SPI fixed-point conversion round-trip
static void test_spi_fixed_point(void) {
    printf("\n--- Test: SPI Fixed-Point Round-Trip ---\n");

    #define SPI_FORCE_SCALE 16384
    #define SPI_VELOCITY_SCALE 256

    float test_forces[] = {0.0f, 1.0f, -1.0f, 0.5f, -0.5f, 0.001f};
    for (int i = 0; i < 6; i++) {
        float original = test_forces[i];
        int16_t fixed = (int16_t)(original * SPI_FORCE_SCALE);
        float recovered = (float)fixed / SPI_FORCE_SCALE;

        char msg[64];
        snprintf(msg, sizeof(msg), "Force Q1.14 round-trip (%.4f)", original);
        ASSERT_NEAR(recovered, original, 0.0002f, msg);  // ~14-bit precision
    }

    float test_velocities[] = {0.0f, 10.0f, -10.0f, 0.5f, 100.0f};
    for (int i = 0; i < 5; i++) {
        float original = test_velocities[i];
        int16_t fixed = (int16_t)(original * SPI_VELOCITY_SCALE);
        float recovered = (float)fixed / SPI_VELOCITY_SCALE;

        char msg[64];
        snprintf(msg, sizeof(msg), "Velocity Q7.8 round-trip (%.4f)", original);
        ASSERT_NEAR(recovered, original, 0.01f, msg);
    }
}

// ─── Main ───

int main(void) {
    printf("=== G29-Xbox Bridge: VM Test Harness ===\n");
    printf("Running on native macOS (not ARM)\n\n");

    init_trig_table();

    test_simple_spring();
    test_spring_with_offset();
    test_multi_axis();
    test_trig_accuracy();
    test_af_accumulate();
    test_spi_fixed_point();

    printf("\n=== Results: %d passed, %d failed ===\n",
           tests_passed, tests_failed);

    return tests_failed > 0 ? 1 : 0;
}
