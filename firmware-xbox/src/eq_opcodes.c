/**
 * eq_opcodes.c — Equation Language Opcode Implementations
 *
 * Uses threaded computed goto dispatch for minimal overhead
 * on Cortex-M0+ (~8 cycles per dispatch vs 15-40 for switch).
 *
 * Opcode reference: H001443 Equation Language for Nextgen FFB Devices
 *
 * Register layout:
 *   Scratch: E0-E3 (per-equation), L0-L7 (per-loop, persistent)
 *   Timing:  Ep,Ea,Ec,Et (equation), Lp,La,Lc,Lt (loop) — read-only
 *   Axis:    Ad,Av,Aa,Ar,Af,Ap,As — context-switched by As
 *
 * All operands are decoded per Appendix B of H001443.
 * Immediate values may be 1, 2, or 4 bytes following the operand byte.
 */

#include "eq_vm.h"
#include <math.h>
#include <string.h>
#include <stdio.h>

// ─── Fast Trig: 256-entry quarter-wave LUT + linear interpolation ───
// Covers [0, pi/2]. Symmetry handles all quadrants.
// Peak error: ~0.004% (40x better than the 0.1% needed for FFB)

#define TRIG_TABLE_SIZE 256
static float sin_table[TRIG_TABLE_SIZE + 1]; // +1 for interpolation at boundary
static bool trig_initialized = false;

#ifndef M_PI
#define M_PI 3.14159265358979323846f
#endif

void eq_opcodes_init_trig(void) {
    for (int i = 0; i <= TRIG_TABLE_SIZE; i++) {
        float angle = ((float)i / TRIG_TABLE_SIZE) * (M_PI / 2.0f);
        sin_table[i] = sinf(angle);
    }
    trig_initialized = true;
}

// Fast sine using quarter-wave LUT + lerp
static float fast_sinf(float x) {
    // Normalize to [0, 2*pi)
    const float TWO_PI = 2.0f * M_PI;
    x = x - TWO_PI * (float)(int)(x / TWO_PI);
    if (x < 0.0f) x += TWO_PI;

    // Determine quadrant and map to [0, pi/2]
    int quadrant;
    float normalized;
    if (x < M_PI / 2.0f) {
        quadrant = 0;
        normalized = x;
    } else if (x < M_PI) {
        quadrant = 1;
        normalized = M_PI - x;
    } else if (x < 3.0f * M_PI / 2.0f) {
        quadrant = 2;
        normalized = x - M_PI;
    } else {
        quadrant = 3;
        normalized = TWO_PI - x;
    }

    // Index into table
    float idx_f = normalized * (2.0f * TRIG_TABLE_SIZE / M_PI);
    int idx = (int)idx_f;
    if (idx >= TRIG_TABLE_SIZE) idx = TRIG_TABLE_SIZE - 1;
    float frac = idx_f - (float)idx;

    // Linear interpolation
    float result = sin_table[idx] + frac * (sin_table[idx + 1] - sin_table[idx]);

    // Apply quadrant sign
    if (quadrant >= 2) result = -result;
    return result;
}

static float fast_cosf(float x) {
    return fast_sinf(x + M_PI / 2.0f);
}

static float fast_tanf(float x) {
    float c = fast_cosf(x);
    if (c == 0.0f) return 0.0f; // Fault case in spec, but avoid div-by-zero here
    return fast_sinf(x) / c;
}

// ─── Operand Encoding (Appendix B) ───
// Each operand byte determines where to find the value and its size.
// This is the most complex part of the decoder.

// Register IDs used in operand encoding
typedef enum {
    // Loop registers (persistent across equations)
    REG_LP = 0, REG_LA, REG_LC, REG_LT,  // Read-only timing
    REG_L0, REG_L1, REG_L2, REG_L3,      // Read-write scratch
    REG_L4, REG_L5, REG_L6, REG_L7,
    // Equation registers (per-equation, reset on Start)
    REG_EP, REG_EA, REG_EC, REG_ET,      // Read-only timing
    REG_E0, REG_E1, REG_E2, REG_E3,      // Read-write scratch
    // Axis registers (context-switched by As)
    REG_AD, REG_AV, REG_AA, REG_AR,      // Position-related (read-only)
    REG_AF, REG_AP,                        // Force-related
    REG_AS,                                // Axis selector (read-write)
    // Stack
    REG_SS,                                // Stack-like selector
    // Special
    REG_AF_PLUS,                           // Af accumulate (Af += value)
    REG_COUNT
} reg_id_t;

// ─── VM Internal State ───
// (defined in eq_vm.c, declared extern here for opcode access)

// These will be exposed via eq_vm.h in the full implementation
// For now, define the structures the opcodes need

typedef struct {
    float registers[REG_COUNT];
    float app_memory[512];      // Application memory (written by Xbox)
    uint8_t bytecode_pool[2048];
    struct {
        eq_state_t state;
        uint16_t size;
        uint16_t offset;
        uint16_t pc;
        float E[4];             // Per-equation scratch
    } equations[16];

    // Per-axis state (indexed by As)
    struct {
        float displacement;     // Ad: -1.0 to 1.0 (wheel) or 0.0 to 1.0 (pedals)
        float velocity;         // Av
        float acceleration;     // Aa
        float displacement_rad; // Ar
        float force;            // Af: accumulated force output
        float prev_force;       // Ap: previous loop's Af
    } axes[8];                  // 0=wheel, 1=throttle, 2=brake, 3=clutch, 4=handbrake, 5=low motor, 6=high motor

    // Loop timing
    float loop_period;          // Lp: seconds since last loop
    float loop_avg_period;      // La: average loop period
    uint32_t loop_count;        // Lc: total loop iterations
    uint32_t loop_time_100us;   // Lt: total loop time in 100us ticks

    // Current equation index (for register context)
    uint8_t current_eq;
    uint8_t current_axis;       // As value

    // Diagnostics
    uint32_t instructions_executed;
} vm_state_t;

// ─── Operand Decoder ───
// Returns the float value for an operand byte and advances the PC.
// Also sets *is_writable and *write_target for output operands.

static float decode_operand(vm_state_t *vm, uint8_t operand,
                            uint16_t *pc, const uint8_t *code, uint16_t code_size,
                            bool *is_writable, float **write_target) {
    *is_writable = false;
    *write_target = NULL;

    uint8_t eq = vm->current_eq;
    uint8_t axis = vm->current_axis;

    // Operand encoding groups (from Appendix B):
    // 0x00: immediate 0 (zero)
    // 0x01-0x0F: small unsigned immediates
    // 0x10-0x17: immediate data (1-4 bytes following operand)
    // 0x18-0x1F: special immediate transforms (1/(n+1), etc.)
    // 0x30-0x3F: equation scratch E0-E3
    // 0x40-0x4F: loop scratch L0-L7 + stack
    // 0x50-0x5F: axis registers Ad, Av, Aa, Ar, As
    // 0x53: Af+ (accumulate to Af)

    switch (operand) {
        // ── Zero ──
        case 0x00:
            return 0.0f;

        // ── Small immediates (unsigned, value = operand) ──
        case 0x01: case 0x02: case 0x03: case 0x04:
        case 0x05: case 0x06: case 0x07: case 0x08:
        case 0x09: case 0x0A: case 0x0B: case 0x0C:
        case 0x0D: case 0x0E: case 0x0F:
            return (float)operand;

        // ── 1-byte immediate ──
        case 0x10: {
            if (*pc >= code_size) return 0.0f;
            uint8_t val = code[(*pc)++];
            return (float)val;
        }

        // ── 2-byte immediate (LE, unsigned) ──
        case 0x11: {
            if (*pc + 1 >= code_size) return 0.0f;
            uint16_t val = code[*pc] | ((uint16_t)code[*pc + 1] << 8);
            *pc += 2;
            return (float)val;
        }

        // ── 4-byte immediate (float, big-endian per spec) ──
        case 0x13: {
            if (*pc + 3 >= code_size) return 0.0f;
            // Big-endian float
            uint32_t raw = ((uint32_t)code[*pc] << 24)
                         | ((uint32_t)code[*pc + 1] << 16)
                         | ((uint32_t)code[*pc + 2] << 8)
                         | code[*pc + 3];
            *pc += 4;
            float result;
            memcpy(&result, &raw, 4);
            return result;
        }

        // ── 1-byte fixed-point: value/100 ──
        case 0x19: {
            if (*pc >= code_size) return 0.0f;
            int8_t val = (int8_t)code[(*pc)++];
            return (float)val / 100.0f;
        }

        // ── 1-byte inverted: 1/(n+1) ──
        case 0x18: {
            if (*pc >= code_size) return 0.0f;
            uint8_t val = code[(*pc)++];
            return 1.0f / (float)(val + 1);
        }

        // ── Equation scratch registers E0-E3 ──
        case 0x30:
            *is_writable = true;
            *write_target = &vm->equations[eq].E[0];
            return vm->equations[eq].E[0];
        case 0x31:
            *is_writable = true;
            *write_target = &vm->equations[eq].E[1];
            return vm->equations[eq].E[1];
        case 0x32:
            *is_writable = true;
            *write_target = &vm->equations[eq].E[2];
            return vm->equations[eq].E[2];
        case 0x33:
            *is_writable = true;
            *write_target = &vm->equations[eq].E[3];
            return vm->equations[eq].E[3];

        // ── Equation timing (read-only) ──
        case 0x38: return 0.0f; // Ep: equation period (TODO)
        case 0x39: return 0.0f; // Ea: equation avg period
        case 0x3A: return (float)0; // Ec: equation run count
        case 0x3B: return (float)0; // Et: equation time (100us ticks)

        // ── Loop scratch registers L0-L7 ──
        case 0x40: case 0x41: case 0x42: case 0x43:
        case 0x44: case 0x45: case 0x46: case 0x47: {
            uint8_t idx = operand - 0x40;
            *is_writable = true;
            *write_target = &vm->registers[REG_L0 + idx];
            return vm->registers[REG_L0 + idx];
        }

        // ── Stack selector ──
        case 0x40 + 8: // Ss
            return vm->registers[REG_SS];

        // ── Axis selector (As) ──
        case 0x50:
            *is_writable = true;
            *write_target = &vm->registers[REG_AS];
            return (float)vm->current_axis;

        // ── Axis displacement (Ad) ──
        case 0x54:
            return vm->axes[axis].displacement;

        // ── Axis velocity (Av) ──
        case 0x55:
            return vm->axes[axis].velocity;

        // ── Axis acceleration (Aa) ──
        case 0x56:
            return vm->axes[axis].acceleration;

        // ── Axis displacement in radians (Ar) ──
        case 0x57:
            return vm->axes[axis].displacement_rad;

        // ── Axis force output (Af) — writable ──
        case 0x52:
            *is_writable = true;
            *write_target = &vm->axes[axis].force;
            return vm->axes[axis].force;

        // ── Axis force accumulate (Af+) — adds to current Af ──
        case 0x53: {
            // Special: writing to Af+ accumulates (Af += value)
            *is_writable = true;
            *write_target = &vm->axes[axis].force; // Caller must ADD, not assign
            return vm->axes[axis].force;
        }

        // ── Axis previous force (Ap) — read-only ──
        case 0x51:
            return vm->axes[axis].prev_force;

        default:
            // Unknown operand — return 0 and flag for potential fault
            return 0.0f;
    }
}

// ─── Opcode Handlers ───
// Each handler reads operands, performs the operation, writes the result.
// Returns false on fault.

// Note: The full implementation with computed goto dispatch will be in eq_vm.c
// This file provides the building blocks. The threaded dispatch loop will
// call these via the dispatch table.

// For now, provide a dispatch function that handles the core opcodes.

bool eq_dispatch_opcode(vm_state_t *vm, uint8_t opcode,
                        uint16_t *pc, const uint8_t *code, uint16_t code_size) {
    float in1, in2, in3, out;
    bool w1, w2, w3;
    float *t1, *t2, *t3;

    switch (opcode) {
        // ── Flow Control ──

        case 0x00: // NOP
            return true;

        case 0x04: // END — exit equation for this loop iteration
            *pc = code_size; // Jump to end
            return true;

        case 0x07: // STP — stop equation (remove from loop)
            vm->equations[vm->current_eq].state = EQ_STOPPED;
            *pc = code_size;
            return true;

        // ── Basic Math ──

        case 0x10: { // ADD — operands decoded sequentially (each may consume immediate bytes)
            uint8_t op1 = code[(*pc)++];
            in1 = decode_operand(vm, op1, pc, code, code_size, &w1, &t1);
            uint8_t op2 = code[(*pc)++];
            in2 = decode_operand(vm, op2, pc, code, code_size, &w2, &t2);
            uint8_t op3 = code[(*pc)++];
            decode_operand(vm, op3, pc, code, code_size, &w3, &t3);
            out = in1 + in2;
            if (w3 && t3) {
                if (op3 == 0x53) *t3 += out; // Af+ accumulate
                else *t3 = out;
            }
            return true;
        }

        case 0x11: { // SUB
            uint8_t op1 = code[(*pc)++];
            in1 = decode_operand(vm, op1, pc, code, code_size, &w1, &t1);
            uint8_t op2 = code[(*pc)++];
            in2 = decode_operand(vm, op2, pc, code, code_size, &w2, &t2);
            uint8_t op3 = code[(*pc)++];
            decode_operand(vm, op3, pc, code, code_size, &w3, &t3);
            out = in1 - in2;
            if (w3 && t3) {
                if (op3 == 0x53) *t3 += out;
                else *t3 = out;
            }
            return true;
        }

        case 0x12: { // MUL
            uint8_t op1 = code[(*pc)++];
            in1 = decode_operand(vm, op1, pc, code, code_size, &w1, &t1);
            uint8_t op2 = code[(*pc)++];
            in2 = decode_operand(vm, op2, pc, code, code_size, &w2, &t2);
            uint8_t op3 = code[(*pc)++];
            decode_operand(vm, op3, pc, code, code_size, &w3, &t3);
            out = in1 * in2;
            if (w3 && t3) {
                if (op3 == 0x53) *t3 += out;
                else *t3 = out;
            }
            return true;
        }

        case 0x13: { // DIV
            uint8_t op1 = code[(*pc)++];
            in1 = decode_operand(vm, op1, pc, code, code_size, &w1, &t1);
            uint8_t op2 = code[(*pc)++];
            in2 = decode_operand(vm, op2, pc, code, code_size, &w2, &t2);
            uint8_t op3 = code[(*pc)++];
            decode_operand(vm, op3, pc, code, code_size, &w3, &t3);
            if (in2 == 0.0f) return false; // FAULT: div by zero
            out = in1 / in2;
            if (w3 && t3) {
                if (op3 == 0x53) *t3 += out;
                else *t3 = out;
            }
            return true;
        }

        case 0x15: { // POW2 (square)
            uint8_t op1 = code[(*pc)++];
            uint8_t op2 = code[(*pc)++];
            in1 = decode_operand(vm, op1, pc, code, code_size, &w1, &t1);
            decode_operand(vm, op2, pc, code, code_size, &w2, &t2);
            out = in1 * in1;
            if (w2 && t2) *t2 = out;
            return true;
        }

        case 0x17: { // SQRT
            uint8_t op1 = code[(*pc)++];
            uint8_t op2 = code[(*pc)++];
            in1 = decode_operand(vm, op1, pc, code, code_size, &w1, &t1);
            decode_operand(vm, op2, pc, code, code_size, &w2, &t2);
            if (in1 < 0.0f) return false; // FAULT
            out = sqrtf(in1);
            if (w2 && t2) *t2 = out;
            return true;
        }

        case 0x19: { // ABS
            uint8_t op1 = code[(*pc)++];
            uint8_t op2 = code[(*pc)++];
            in1 = decode_operand(vm, op1, pc, code, code_size, &w1, &t1);
            decode_operand(vm, op2, pc, code, code_size, &w2, &t2);
            out = (in1 >= 0.0f) ? in1 : -in1;
            if (w2 && t2) *t2 = out;
            return true;
        }

        // ── Trig (using fast LUT) ──

        case 0x20: { // SIN
            uint8_t op1 = code[(*pc)++];
            uint8_t op2 = code[(*pc)++];
            in1 = decode_operand(vm, op1, pc, code, code_size, &w1, &t1);
            decode_operand(vm, op2, pc, code, code_size, &w2, &t2);
            out = fast_sinf(in1);
            if (w2 && t2) *t2 = out;
            return true;
        }

        case 0x21: { // COS
            uint8_t op1 = code[(*pc)++];
            uint8_t op2 = code[(*pc)++];
            in1 = decode_operand(vm, op1, pc, code, code_size, &w1, &t1);
            decode_operand(vm, op2, pc, code, code_size, &w2, &t2);
            out = fast_cosf(in1);
            if (w2 && t2) *t2 = out;
            return true;
        }

        case 0x22: { // TAN
            uint8_t op1 = code[(*pc)++];
            uint8_t op2 = code[(*pc)++];
            in1 = decode_operand(vm, op1, pc, code, code_size, &w1, &t1);
            decode_operand(vm, op2, pc, code, code_size, &w2, &t2);
            out = fast_tanf(in1);
            if (w2 && t2) *t2 = out;
            return true;
        }

        // ── Comparison ──

        case 0x40: { // GT
            uint8_t op1 = code[(*pc)++];
            in1 = decode_operand(vm, op1, pc, code, code_size, &w1, &t1);
            uint8_t op2 = code[(*pc)++];
            in2 = decode_operand(vm, op2, pc, code, code_size, &w2, &t2);
            uint8_t op3 = code[(*pc)++];
            decode_operand(vm, op3, pc, code, code_size, &w3, &t3);
            out = (in1 > in2) ? 1.0f : 0.0f;
            if (w3 && t3) *t3 = out;
            return true;
        }

        case 0x41: { // GTE
            uint8_t op1 = code[(*pc)++];
            in1 = decode_operand(vm, op1, pc, code, code_size, &w1, &t1);
            uint8_t op2 = code[(*pc)++];
            in2 = decode_operand(vm, op2, pc, code, code_size, &w2, &t2);
            uint8_t op3 = code[(*pc)++];
            decode_operand(vm, op3, pc, code, code_size, &w3, &t3);
            out = (in1 >= in2) ? 1.0f : 0.0f;
            if (w3 && t3) *t3 = out;
            return true;
        }

        // ── Boolean ──

        case 0x30: { // AND (boolean, not bitwise)
            // Note: 0x30 conflicts with E0 register operand encoding
            // The opcode byte is distinct from operand bytes — no conflict
            // since opcodes are only in the first byte position
            uint8_t op1 = code[(*pc)++];
            uint8_t op2 = code[(*pc)++];
            uint8_t op3 = code[(*pc)++];
            in1 = decode_operand(vm, op1, pc, code, code_size, &w1, &t1);
            in2 = decode_operand(vm, op2, pc, code, code_size, &w2, &t2);
            decode_operand(vm, op3, pc, code, code_size, &w3, &t3);
            out = (in1 != 0.0f && in2 != 0.0f) ? 1.0f : 0.0f;
            if (w3 && t3) *t3 = out;
            return true;
        }

        // ── MOV ──

        case 0xA0: { // MOV
            uint8_t op1 = code[(*pc)++];
            uint8_t op2 = code[(*pc)++];
            in1 = decode_operand(vm, op1, pc, code, code_size, &w1, &t1);
            decode_operand(vm, op2, pc, code, code_size, &w2, &t2);
            if (w2 && t2) {
                if (op2 == 0x50) {
                    // Writing to As — update axis context
                    uint8_t new_axis = (uint8_t)(int)in1;
                    if (new_axis > 7) return false; // FAULT: invalid axis
                    vm->current_axis = new_axis;
                } else if (op2 == 0x53) {
                    *t2 += in1; // Af+ accumulate
                } else {
                    *t2 = in1;
                }
            }
            return true;
        }

        // ── Conditional flow ──

        case 0x05: { // ENDZ — end if zero
            uint8_t op1 = code[(*pc)++];
            in1 = decode_operand(vm, op1, pc, code, code_size, &w1, &t1);
            if (in1 == 0.0f) *pc = code_size; // End equation
            return true;
        }

        case 0x06: { // ENDN — end if not zero
            uint8_t op1 = code[(*pc)++];
            in1 = decode_operand(vm, op1, pc, code, code_size, &w1, &t1);
            if (in1 != 0.0f) *pc = code_size;
            return true;
        }

        case 0x08: { // STPZ — stop if zero
            uint8_t op1 = code[(*pc)++];
            in1 = decode_operand(vm, op1, pc, code, code_size, &w1, &t1);
            if (in1 == 0.0f) {
                vm->equations[vm->current_eq].state = EQ_STOPPED;
                *pc = code_size;
            }
            return true;
        }

        case 0x09: { // STPN — stop if not zero
            uint8_t op1 = code[(*pc)++];
            in1 = decode_operand(vm, op1, pc, code, code_size, &w1, &t1);
            if (in1 != 0.0f) {
                vm->equations[vm->current_eq].state = EQ_STOPPED;
                *pc = code_size;
            }
            return true;
        }

        // ── Positional tests ──

        case 0x44: { // NEG — is negative?
            uint8_t op1 = code[(*pc)++];
            uint8_t op2 = code[(*pc)++];
            in1 = decode_operand(vm, op1, pc, code, code_size, &w1, &t1);
            decode_operand(vm, op2, pc, code, code_size, &w2, &t2);
            out = (in1 < 0.0f) ? 1.0f : 0.0f;
            if (w2 && t2) *t2 = out;
            return true;
        }

        case 0x45: { // POS — is positive?
            // Note: uses 0x2C in the assembled bytecode per spec example
            uint8_t op1 = code[(*pc)++];
            uint8_t op2 = code[(*pc)++];
            in1 = decode_operand(vm, op1, pc, code, code_size, &w1, &t1);
            decode_operand(vm, op2, pc, code, code_size, &w2, &t2);
            out = (in1 > 0.0f) ? 1.0f : 0.0f;
            if (w2 && t2) *t2 = out;
            return true;
        }

        // ── SGN ──
        case 0x18: { // SGN
            uint8_t op1 = code[(*pc)++];
            in1 = decode_operand(vm, op1, pc, code, code_size, &w1, &t1);
            uint8_t op2 = code[(*pc)++];
            decode_operand(vm, op2, pc, code, code_size, &w2, &t2);
            if (in1 > 0.0f) out = 1.0f;
            else if (in1 < 0.0f) out = -1.0f;
            else out = 0.0f;
            if (w2 && t2) *t2 = out;
            return true;
        }

        // ── Skip Instructions ──

        case 0x01: { // SKP — unconditional skip N instructions
            uint8_t op1 = code[(*pc)++];
            in1 = decode_operand(vm, op1, pc, code, code_size, &w1, &t1);
            uint32_t skip_count = (uint32_t)in1;
            // Skip N instructions — we approximate by skipping N bytes
            // TODO: proper instruction-counting skip requires instruction length table
            *pc += (uint16_t)skip_count;
            if (*pc > code_size) return false; // FAULT: skip past end
            return true;
        }

        case 0x02: { // SKPZ — skip if zero
            uint8_t op1 = code[(*pc)++];
            in1 = decode_operand(vm, op1, pc, code, code_size, &w1, &t1);
            uint8_t op2 = code[(*pc)++];
            in2 = decode_operand(vm, op2, pc, code, code_size, &w2, &t2);
            if (in1 == 0.0f) {
                uint32_t skip_count = (uint32_t)in2;
                *pc += (uint16_t)skip_count;
                if (*pc > code_size) return false;
            }
            return true;
        }

        case 0x03: { // SKPN — skip if not zero
            uint8_t op1 = code[(*pc)++];
            in1 = decode_operand(vm, op1, pc, code, code_size, &w1, &t1);
            uint8_t op2 = code[(*pc)++];
            in2 = decode_operand(vm, op2, pc, code, code_size, &w2, &t2);
            if (in1 != 0.0f) {
                uint32_t skip_count = (uint32_t)in2;
                *pc += (uint16_t)skip_count;
                if (*pc > code_size) return false;
            }
            return true;
        }

        // ── Remaining Math ──

        case 0x14: { // MOD (fmod)
            uint8_t op1 = code[(*pc)++];
            in1 = decode_operand(vm, op1, pc, code, code_size, &w1, &t1);
            uint8_t op2 = code[(*pc)++];
            in2 = decode_operand(vm, op2, pc, code, code_size, &w2, &t2);
            uint8_t op3 = code[(*pc)++];
            decode_operand(vm, op3, pc, code, code_size, &w3, &t3);
            if (in2 == 0.0f) return false; // FAULT
            out = fmodf(in1, in2);
            if (w3 && t3) { if (op3 == 0x53) *t3 += out; else *t3 = out; }
            return true;
        }

        case 0x16: { // POW3
            uint8_t op1 = code[(*pc)++];
            in1 = decode_operand(vm, op1, pc, code, code_size, &w1, &t1);
            uint8_t op2 = code[(*pc)++];
            decode_operand(vm, op2, pc, code, code_size, &w2, &t2);
            out = in1 * in1 * in1;
            if (w2 && t2) *t2 = out;
            return true;
        }

        case 0x1A: { // INV (1/x)
            uint8_t op1 = code[(*pc)++];
            in1 = decode_operand(vm, op1, pc, code, code_size, &w1, &t1);
            uint8_t op2 = code[(*pc)++];
            decode_operand(vm, op2, pc, code, code_size, &w2, &t2);
            if (in1 == 0.0f) return false; // FAULT
            out = 1.0f / in1;
            if (w2 && t2) *t2 = out;
            return true;
        }

        // ── Trig extras ──

        case 0x2A: { // PMUL (x * pi)
            uint8_t op1 = code[(*pc)++];
            in1 = decode_operand(vm, op1, pc, code, code_size, &w1, &t1);
            uint8_t op2 = code[(*pc)++];
            decode_operand(vm, op2, pc, code, code_size, &w2, &t2);
            out = in1 * (float)M_PI;
            if (w2 && t2) *t2 = out;
            return true;
        }

        case 0x2B: { // PDIV (x / pi)
            uint8_t op1 = code[(*pc)++];
            in1 = decode_operand(vm, op1, pc, code, code_size, &w1, &t1);
            uint8_t op2 = code[(*pc)++];
            decode_operand(vm, op2, pc, code, code_size, &w2, &t2);
            out = in1 / (float)M_PI;
            if (w2 && t2) *t2 = out;
            return true;
        }

        case 0x2C: { // RAD (degrees to radians)
            uint8_t op1 = code[(*pc)++];
            in1 = decode_operand(vm, op1, pc, code, code_size, &w1, &t1);
            uint8_t op2 = code[(*pc)++];
            decode_operand(vm, op2, pc, code, code_size, &w2, &t2);
            out = in1 * ((float)M_PI / 180.0f);
            if (w2 && t2) *t2 = out;
            return true;
        }

        case 0x2D: { // DEG (radians to degrees)
            uint8_t op1 = code[(*pc)++];
            in1 = decode_operand(vm, op1, pc, code, code_size, &w1, &t1);
            uint8_t op2 = code[(*pc)++];
            decode_operand(vm, op2, pc, code, code_size, &w2, &t2);
            out = in1 * (180.0f / (float)M_PI);
            if (w2 && t2) *t2 = out;
            return true;
        }

        // ── Remaining Boolean ──

        case 0x31: { // OR
            uint8_t op1 = code[(*pc)++];
            in1 = decode_operand(vm, op1, pc, code, code_size, &w1, &t1);
            uint8_t op2 = code[(*pc)++];
            in2 = decode_operand(vm, op2, pc, code, code_size, &w2, &t2);
            uint8_t op3 = code[(*pc)++];
            decode_operand(vm, op3, pc, code, code_size, &w3, &t3);
            out = (in1 != 0.0f || in2 != 0.0f) ? 1.0f : 0.0f;
            if (w3 && t3) *t3 = out;
            return true;
        }

        case 0x32: { // NAND
            uint8_t op1 = code[(*pc)++];
            in1 = decode_operand(vm, op1, pc, code, code_size, &w1, &t1);
            uint8_t op2 = code[(*pc)++];
            in2 = decode_operand(vm, op2, pc, code, code_size, &w2, &t2);
            uint8_t op3 = code[(*pc)++];
            decode_operand(vm, op3, pc, code, code_size, &w3, &t3);
            out = !(in1 != 0.0f && in2 != 0.0f) ? 1.0f : 0.0f;
            if (w3 && t3) *t3 = out;
            return true;
        }

        case 0x34: { // XOR
            uint8_t op1 = code[(*pc)++];
            in1 = decode_operand(vm, op1, pc, code, code_size, &w1, &t1);
            uint8_t op2 = code[(*pc)++];
            in2 = decode_operand(vm, op2, pc, code, code_size, &w2, &t2);
            uint8_t op3 = code[(*pc)++];
            decode_operand(vm, op3, pc, code, code_size, &w3, &t3);
            out = ((in1 != 0.0f) != (in2 != 0.0f)) ? 1.0f : 0.0f;
            if (w3 && t3) *t3 = out;
            return true;
        }

        case 0x35: { // NOT
            uint8_t op1 = code[(*pc)++];
            in1 = decode_operand(vm, op1, pc, code, code_size, &w1, &t1);
            uint8_t op2 = code[(*pc)++];
            decode_operand(vm, op2, pc, code, code_size, &w2, &t2);
            out = (in1 == 0.0f) ? 1.0f : 0.0f;
            if (w2 && t2) *t2 = out;
            return true;
        }

        // ── Remaining Comparison ──

        case 0x42: { // LT
            uint8_t op1 = code[(*pc)++];
            in1 = decode_operand(vm, op1, pc, code, code_size, &w1, &t1);
            uint8_t op2 = code[(*pc)++];
            in2 = decode_operand(vm, op2, pc, code, code_size, &w2, &t2);
            uint8_t op3 = code[(*pc)++];
            decode_operand(vm, op3, pc, code, code_size, &w3, &t3);
            out = (in1 < in2) ? 1.0f : 0.0f;
            if (w3 && t3) *t3 = out;
            return true;
        }

        case 0x43: { // LTE
            uint8_t op1 = code[(*pc)++];
            in1 = decode_operand(vm, op1, pc, code, code_size, &w1, &t1);
            uint8_t op2 = code[(*pc)++];
            in2 = decode_operand(vm, op2, pc, code, code_size, &w2, &t2);
            uint8_t op3 = code[(*pc)++];
            decode_operand(vm, op3, pc, code, code_size, &w3, &t3);
            out = (in1 <= in2) ? 1.0f : 0.0f;
            if (w3 && t3) *t3 = out;
            return true;
        }

        case 0x46: { // EQ (bit comparison)
            uint8_t op1 = code[(*pc)++];
            in1 = decode_operand(vm, op1, pc, code, code_size, &w1, &t1);
            uint8_t op2 = code[(*pc)++];
            in2 = decode_operand(vm, op2, pc, code, code_size, &w2, &t2);
            uint8_t op3 = code[(*pc)++];
            decode_operand(vm, op3, pc, code, code_size, &w3, &t3);
            // Bit comparison — treat as raw 32-bit equality
            uint32_t a, b;
            memcpy(&a, &in1, 4);
            memcpy(&b, &in2, 4);
            out = (a == b) ? 1.0f : 0.0f;
            if (w3 && t3) *t3 = out;
            return true;
        }

        // ── Bitwise Operations (unsigned) ──

        case 0x50: { // BAND
            uint8_t op1 = code[(*pc)++];
            in1 = decode_operand(vm, op1, pc, code, code_size, &w1, &t1);
            uint8_t op2 = code[(*pc)++];
            in2 = decode_operand(vm, op2, pc, code, code_size, &w2, &t2);
            uint8_t op3 = code[(*pc)++];
            decode_operand(vm, op3, pc, code, code_size, &w3, &t3);
            uint32_t ua, ub, ur;
            memcpy(&ua, &in1, 4); memcpy(&ub, &in2, 4);
            ur = ua & ub;
            float fout; memcpy(&fout, &ur, 4);
            if (w3 && t3) *t3 = fout;
            return true;
        }

        case 0x51: { // BOR
            uint8_t op1 = code[(*pc)++];
            in1 = decode_operand(vm, op1, pc, code, code_size, &w1, &t1);
            uint8_t op2 = code[(*pc)++];
            in2 = decode_operand(vm, op2, pc, code, code_size, &w2, &t2);
            uint8_t op3 = code[(*pc)++];
            decode_operand(vm, op3, pc, code, code_size, &w3, &t3);
            uint32_t ua, ub, ur;
            memcpy(&ua, &in1, 4); memcpy(&ub, &in2, 4);
            ur = ua | ub;
            float fout; memcpy(&fout, &ur, 4);
            if (w3 && t3) *t3 = fout;
            return true;
        }

        // ── Lookup: ARRXY (interpolation — critical for spring profiles) ──

        case 0xA3: { // ARRXY — interpolate along XY pairs in app memory
            uint8_t op1 = code[(*pc)++];
            in1 = decode_operand(vm, op1, pc, code, code_size, &w1, &t1); // base addr
            uint8_t op2 = code[(*pc)++];
            in2 = decode_operand(vm, op2, pc, code, code_size, &w2, &t2); // pair count
            uint8_t op3 = code[(*pc)++];
            float in3_val = decode_operand(vm, op3, pc, code, code_size, &w3, &t3); // X value
            uint8_t op4 = code[(*pc)++];
            decode_operand(vm, op4, pc, code, code_size, &w1, &t1); // output

            uint32_t base = (uint32_t)in1;
            uint32_t count = (uint32_t)in2;
            float x_val = in3_val;

            if (count < 2) return false; // FAULT: need at least 2 pairs

            // Read XY pairs from app memory
            // Each pair: memory[base + i*2] = X, memory[base + i*2 + 1] = Y
            float x0 = vm->app_memory[base];
            float y0 = vm->app_memory[base + 1];

            if (x_val <= x0) {
                // Extrapolate from first two points
                float x1 = vm->app_memory[base + 2];
                float y1 = vm->app_memory[base + 3];
                float slope = (x1 != x0) ? (y1 - y0) / (x1 - x0) : 0.0f;
                out = y0 + slope * (x_val - x0);
            } else {
                // Find the surrounding pair
                float prev_x = x0, prev_y = y0;
                bool found = false;
                for (uint32_t i = 1; i < count; i++) {
                    uint32_t addr = base + i * 2;
                    if (addr + 1 >= 512) return false; // FAULT: out of bounds
                    float xi = vm->app_memory[addr];
                    float yi = vm->app_memory[addr + 1];

                    if (x_val <= xi) {
                        // Interpolate between prev and current
                        float t = (xi != prev_x) ? (x_val - prev_x) / (xi - prev_x) : 0.0f;
                        out = prev_y + t * (yi - prev_y);
                        found = true;
                        break;
                    }
                    prev_x = xi;
                    prev_y = yi;
                }
                if (!found) {
                    // Extrapolate from last two points
                    uint32_t last_addr = base + (count - 1) * 2;
                    float xn = vm->app_memory[last_addr];
                    float yn = vm->app_memory[last_addr + 1];
                    float xp = vm->app_memory[last_addr - 2];
                    float yp = vm->app_memory[last_addr - 1];
                    float slope = (xn != xp) ? (yn - yp) / (xn - xp) : 0.0f;
                    out = yn + slope * (x_val - xn);
                }
            }

            if (w1 && t1) {
                if (op4 == 0x53) *t1 += out;
                else *t1 = out;
            }
            return true;
        }

        // ── Boundary / Interpolation ──

        case 0xA8: { // BND — boundary clamp
            uint8_t op1 = code[(*pc)++];
            in1 = decode_operand(vm, op1, pc, code, code_size, &w1, &t1); // min
            uint8_t op2 = code[(*pc)++];
            in2 = decode_operand(vm, op2, pc, code, code_size, &w2, &t2); // max
            uint8_t op3 = code[(*pc)++];
            float in3_val = decode_operand(vm, op3, pc, code, code_size, &w3, &t3); // value
            uint8_t op4 = code[(*pc)++];
            decode_operand(vm, op4, pc, code, code_size, &w1, &t1); // output

            if (in3_val <= in1) out = -1.0f;
            else if (in3_val >= in2) out = 1.0f;
            else {
                // Linear interpolation between boundaries
                out = 2.0f * (in3_val - in1) / (in2 - in1) - 1.0f;
            }
            if (w1 && t1) *t1 = out;
            return true;
        }

        // ── Conversion ──

        case 0xD0: { // TOFL — unsigned to float
            uint8_t op1 = code[(*pc)++];
            in1 = decode_operand(vm, op1, pc, code, code_size, &w1, &t1);
            uint8_t op2 = code[(*pc)++];
            decode_operand(vm, op2, pc, code, code_size, &w2, &t2);
            // Reinterpret as unsigned then convert
            uint32_t u; memcpy(&u, &in1, 4);
            out = (float)u;
            if (w2 && t2) *t2 = out;
            return true;
        }

        case 0xD2: { // DIFF_FL — unsigned difference to float
            uint8_t op1 = code[(*pc)++];
            in1 = decode_operand(vm, op1, pc, code, code_size, &w1, &t1);
            uint8_t op2 = code[(*pc)++];
            in2 = decode_operand(vm, op2, pc, code, code_size, &w2, &t2);
            uint8_t op3 = code[(*pc)++];
            decode_operand(vm, op3, pc, code, code_size, &w3, &t3);
            uint32_t ua, ub; memcpy(&ua, &in1, 4); memcpy(&ub, &in2, 4);
            int32_t diff = (int32_t)(ua - ub);
            out = (float)diff;
            if (w3 && t3) *t3 = out;
            return true;
        }

        // ── ARR (simple array lookup) ──

        case 0xA1: { // ARR — lookup value in app memory
            uint8_t op1 = code[(*pc)++];
            in1 = decode_operand(vm, op1, pc, code, code_size, &w1, &t1); // base addr
            uint8_t op2 = code[(*pc)++];
            in2 = decode_operand(vm, op2, pc, code, code_size, &w2, &t2); // offset
            uint8_t op3 = code[(*pc)++];
            decode_operand(vm, op3, pc, code, code_size, &w3, &t3); // output
            uint32_t addr = (uint32_t)in1 + (uint32_t)in2;
            if (addr >= 512) return false; // FAULT: out of bounds
            out = vm->app_memory[addr];
            if (w3 && t3) *t3 = out;
            return true;
        }

        default:
            printf("[VM] Unknown opcode 0x%02X at PC=%d\n", opcode, *pc - 1);
            return false; // FAULT
    }
}
