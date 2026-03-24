/**
 * diagnostics.c — Event Ring Buffer for RP2040 #1 (Xbox + VM)
 */

#include "pico/time.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#define DIAG_RING_SIZE 256

// Event types for Xbox + VM side
#define DIAG_EVT_VM_OVERRUN         0x01
#define DIAG_EVT_VM_EQ_FAULT        0x02
#define DIAG_EVT_VM_EQ_RUNAWAY      0x03
#define DIAG_EVT_VM_TICK_ADAPT      0x04
#define DIAG_EVT_GIP_CMD_IN         0x10
#define DIAG_EVT_GIP_CMD_OUT        0x11
#define DIAG_EVT_GIP_KEEPALIVE      0x12
#define DIAG_EVT_GIP_AUTH_FWD       0x13
#define DIAG_EVT_GIP_QUIESCE        0x14
#define DIAG_EVT_SPI_CRC_ERROR      0x20
#define DIAG_EVT_SPI_TIMEOUT        0x21
#define DIAG_EVT_XBOX_MOUNT         0x30
#define DIAG_EVT_XBOX_UNMOUNT       0x31

typedef struct {
    uint32_t timestamp_ms;
    uint8_t  event_type;
    uint8_t  data1;
    uint8_t  data2;
    uint8_t  data3;
} diag_event_t;

static diag_event_t ring[DIAG_RING_SIZE];
static volatile uint16_t ring_head = 0;
static volatile uint32_t total_events = 0;

// VM-specific counters (accessible from Core 1)
volatile uint32_t diag_vm_overruns = 0;
volatile uint32_t diag_vm_faults = 0;
volatile uint32_t diag_vm_max_tick_us = 0;

void diag_init(void) {
    memset(ring, 0, sizeof(ring));
    ring_head = 0;
    total_events = 0;
    diag_vm_overruns = 0;
    diag_vm_faults = 0;
    diag_vm_max_tick_us = 0;
}

void diag_log(uint8_t event_type, uint8_t d1, uint8_t d2, uint8_t d3) {
    diag_event_t *evt = &ring[ring_head];
    evt->timestamp_ms = to_ms_since_boot(get_absolute_time());
    evt->event_type = event_type;
    evt->data1 = d1;
    evt->data2 = d2;
    evt->data3 = d3;
    ring_head = (ring_head + 1) % DIAG_RING_SIZE;
    total_events++;
}

void diag_dump_recent(uint16_t count) {
    if (count > DIAG_RING_SIZE) count = DIAG_RING_SIZE;
    if (count > total_events) count = (uint16_t)total_events;

    printf("[DIAG] Last %d events (total: %lu):\n", count, total_events);
    printf("[DIAG] VM overruns=%lu faults=%lu max_tick=%luus\n",
           diag_vm_overruns, diag_vm_faults, diag_vm_max_tick_us);

    uint16_t start = (ring_head - count + DIAG_RING_SIZE) % DIAG_RING_SIZE;
    for (uint16_t i = 0; i < count; i++) {
        uint16_t idx = (start + i) % DIAG_RING_SIZE;
        diag_event_t *e = &ring[idx];
        if (e->timestamp_ms == 0 && e->event_type == 0) continue;
        printf("  [%08lu] type=0x%02X d1=0x%02X d2=0x%02X d3=0x%02X\n",
               e->timestamp_ms, e->event_type, e->data1, e->data2, e->data3);
    }
}
