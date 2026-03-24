/**
 * diagnostics.c — Event Ring Buffer for Debug Telemetry
 *
 * SRAM ring buffer capturing the last DIAG_RING_SIZE events.
 * Events are small (8 bytes each) to minimize memory impact.
 * Can be dumped to UART on demand or read by a debugger.
 *
 * Event types cover: USB errors, SPI errors, FFB overruns,
 * connection state changes, watchdog near-misses.
 */

#include "bridge_config.h"
#include "pico/time.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>

// ─── Event Types ───

#define DIAG_EVT_USB_ERROR          0x01
#define DIAG_EVT_USB_CONNECT        0x02
#define DIAG_EVT_USB_DISCONNECT     0x03
#define DIAG_EVT_SPI_CRC_ERROR      0x04
#define DIAG_EVT_SPI_TIMEOUT        0x05
#define DIAG_EVT_FFB_SEND_FAIL      0x06
#define DIAG_EVT_G29_MODE_SWITCH    0x07
#define DIAG_EVT_G29_RENUM_OK       0x08
#define DIAG_EVT_G29_RENUM_FAIL     0x09
#define DIAG_EVT_AUTH_CHALLENGE     0x0A
#define DIAG_EVT_AUTH_RESPONSE      0x0B
#define DIAG_EVT_AUTH_TIMEOUT       0x0C
#define DIAG_EVT_WATCHDOG_WARN      0x0D

// ─── Event Structure (8 bytes) ───

typedef struct {
    uint32_t timestamp_ms;  // Milliseconds since boot
    uint8_t  event_type;    // DIAG_EVT_*
    uint8_t  data1;         // Event-specific
    uint8_t  data2;
    uint8_t  data3;
} diag_event_t;

// ─── Ring Buffer ───

static diag_event_t ring[DIAG_RING_SIZE];
static volatile uint16_t ring_head = 0;  // Next write position
static volatile uint32_t total_events = 0;

// ─── Public API ───

void diag_init(void) {
    memset(ring, 0, sizeof(ring));
    ring_head = 0;
    total_events = 0;
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

    uint16_t start = (ring_head - count + DIAG_RING_SIZE) % DIAG_RING_SIZE;
    for (uint16_t i = 0; i < count; i++) {
        uint16_t idx = (start + i) % DIAG_RING_SIZE;
        diag_event_t *e = &ring[idx];
        if (e->timestamp_ms == 0 && e->event_type == 0) continue;

        printf("  [%08lu] type=0x%02X d1=0x%02X d2=0x%02X d3=0x%02X\n",
               e->timestamp_ms, e->event_type, e->data1, e->data2, e->data3);
    }
}

uint32_t diag_get_total(void) {
    return total_events;
}
