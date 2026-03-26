/**
 * auth_host.c — PowerA PIO-USB Host + Auth Relay (Board X)
 *
 * Runs on Core 1 alongside the cooperative Equation VM.
 * Hosts an intact PowerA Xbox controller via PIO-USB (GPIO 0/1).
 * Acts as minimal GIP host, relays auth challenges/responses between
 * gip_protocol.c (Xbox GIP device on Core 0) and the PowerA.
 *
 * Cross-core communication: auth_challenge/response buffers are
 * double-buffered with atomic ready flags to avoid torn reads.
 */

#include "auth_host.h"
#include "pio_usb.h"
#include "pio_usb_ll.h"
#include "usb_definitions.h"
#include "tusb.h"

#include "pico/stdlib.h"
#include "hardware/watchdog.h"
#include <stdio.h>
#include <string.h>

// ─── GIP Host State Machine ───

typedef enum {
    GIP_HOST_IDLE,
    GIP_HOST_OPENING_ENDPOINTS,
    GIP_HOST_WAIT_ANNOUNCE,
    GIP_HOST_WAIT_IDENTIFY,     // Sent IDENTIFY request, waiting for chunked response
    GIP_HOST_POWERING_ON,       // IDENTIFY complete, sending Power ON
    GIP_HOST_READY,
} gip_host_state_t;

// ─── Device State ───

static bool auth_connected = false;
static uint8_t auth_dev_addr = 0;

// Endpoint info
static uint8_t ep_in_addr = 0;
static uint8_t ep_out_addr = 0;
static bool endpoints_open = false;

// GIP host state
static gip_host_state_t gip_state = GIP_HOST_IDLE;
static uint8_t gip_seq_counters[256];
static uint32_t auth_mount_time = 0;
static bool auth_relay_started = false;

#define AUTH_INIT_DELAY_MS  1000  // Wait 1s after mount before sending to PowerA

// RX buffer for GIP packets from PowerA
static uint8_t gip_rx_buf[64];
static volatile bool rx_pending = false;
static volatile bool rx_needs_retry = false;
static volatile bool rx_needs_start = false;
static volatile uint32_t rx_actual_len = 0;

// TX state
static uint8_t gip_tx_buf[64];
static volatile bool tx_pending = false;

// ─── Cross-Core Auth Buffers ───
// Core 0 writes challenge, Core 1 reads it
// Core 1 writes response, Core 0 reads it

#define AUTH_MAX_PAYLOAD 256

static uint8_t challenge_buf[AUTH_MAX_PAYLOAD];
static volatile uint16_t challenge_len = 0;
static volatile bool challenge_ready = false;  // Set by Core 0, cleared by Core 1

static uint8_t response_buf[AUTH_MAX_PAYLOAD];
static volatile uint16_t response_len = 0;
static volatile bool response_ready = false;   // Set by Core 1, cleared by Core 0

// Auth challenge TX state (chunked to PowerA)
static uint8_t auth_tx_buf[AUTH_MAX_PAYLOAD];
static uint16_t auth_tx_len = 0;
static uint16_t auth_tx_offset = 0;
static bool auth_tx_queued = false;

// Auth response RX state (chunked from PowerA)
static uint8_t auth_rx_buf[AUTH_MAX_PAYLOAD];
static uint16_t auth_rx_len = 0;
static uint16_t auth_rx_offset = 0;
static bool auth_rx_ready = false;

// IDENTIFY response reassembly from controller
static uint8_t identify_rx_buf[512];
static uint16_t identify_rx_offset = 0;
static bool identify_complete = false;
static uint16_t identify_total_raw = 0;     // Total raw bytes expected (from XGIP unhash)
static uint16_t identify_received_raw = 0;  // Cumulative payload bytes received

// Pending ACK queue — deferred when tx_pending blocks immediate send
#define ACK_QUEUE_SIZE 4
typedef struct {
    uint8_t cmd;
    uint8_t flags;
    uint8_t seq;
    uint16_t received;   // Chunk progress: cumulative bytes (0 = simple ACK)
    uint16_t remaining;  // Chunk progress: bytes remaining
    bool valid;
} pending_ack_t;
static pending_ack_t ack_queue[ACK_QUEUE_SIZE];
static uint8_t ack_queue_head = 0;

// Config descriptor buffer
static uint8_t config_desc_buf[256];

// ─── GIP Packet Helpers ───

static uint8_t gip_host_next_seq(uint8_t cmd) {
    uint8_t seq = ++gip_seq_counters[cmd];
    if (seq == 0) seq = ++gip_seq_counters[cmd];
    return seq;
}

static uint8_t gip_build(uint8_t *buf, uint8_t cmd, uint8_t flags,
                          const uint8_t *payload, uint16_t payload_len) {
    buf[0] = cmd;
    buf[1] = flags;
    buf[2] = gip_host_next_seq(cmd);

    uint8_t hdr_len;
    if (payload_len < 128) {
        buf[3] = (uint8_t)payload_len;
        hdr_len = 4;
    } else {
        buf[3] = (uint8_t)(payload_len & 0x7F) | 0x80;
        buf[4] = (uint8_t)(payload_len >> 7);
        hdr_len = 5;
    }

    if (payload && payload_len > 0) {
        uint16_t copy = payload_len;
        if (hdr_len + copy > 64) copy = 64 - hdr_len;
        memcpy(&buf[hdr_len], payload, copy);
    }

    return hdr_len + (payload_len < (64 - hdr_len) ? payload_len : (64 - hdr_len));
}

static bool gip_parse(const uint8_t *pkt, uint16_t pkt_len,
                       uint8_t *cmd, uint8_t *flags, uint8_t *seq,
                       const uint8_t **payload, uint16_t *payload_len) {
    if (pkt_len < 4) return false;

    *cmd = pkt[0];
    *flags = pkt[1];
    *seq = pkt[2];

    uint8_t hdr_len;
    if (!(pkt[3] & 0x80)) {
        *payload_len = pkt[3];
        hdr_len = 4;
    } else {
        if (pkt_len < 5) return false;
        *payload_len = (pkt[3] & 0x7F) | ((uint16_t)pkt[4] << 7);
        hdr_len = 5;
    }

    *payload = &pkt[hdr_len];
    return true;
}

// ─── XGIP Chunk Length Hashing (duplicated from gip_protocol.c) ───
// Needed to reverse-compute raw byte count from XGIP hashed total
// in the controller's chunked IDENTIFY response header.

#define GIP_HOST_MAX_CHUNK_DATA  0x3A  // 58 bytes max payload per chunk

static uint16_t xgip_compute_chunk_length_local(uint16_t data_len) {
    uint16_t i = data_len;
    uint16_t j = 0;
    do {
        if (i < GIP_HOST_MAX_CHUNK_DATA) {
            if ((j / 0x100) != ((j + i) / 0x100))
                j = j + (i | 0x80);
            else
                j = j + i;
            i = 0;
        } else {
            if ((j + GIP_HOST_MAX_CHUNK_DATA > 0x80) && (j + GIP_HOST_MAX_CHUNK_DATA < 0x100))
                j = j + GIP_HOST_MAX_CHUNK_DATA + 0x100;
            else if ((j / 0x100) != ((j + GIP_HOST_MAX_CHUNK_DATA) / 0x100))
                j = j + (GIP_HOST_MAX_CHUNK_DATA | 0x80);
            else
                j = j + GIP_HOST_MAX_CHUNK_DATA;
            i = i - GIP_HOST_MAX_CHUNK_DATA;
        }
    } while (i != 0);
    return j;
}

// Reverse-lookup: find raw byte count that produces a given XGIP hash
static uint16_t xgip_unhash_length(uint16_t hashed) {
    for (uint16_t raw = 1; raw <= 512; raw++) {
        if (xgip_compute_chunk_length_local(raw) == hashed)
            return raw;
    }
    return 0;  // Unknown — fall back to short-chunk detection
}

// Forward declarations
static void on_tx_complete(tuh_xfer_t *xfer);

// ─── GIP ACK Sender ───

// Simple ACK (non-chunked): payload = [0x00, cmd, 0x20, 0,0,0,0,0,0]
static bool send_gip_ack_now(uint8_t acked_cmd, uint8_t acked_flags, uint8_t acked_seq) {
    if (!endpoints_open || ep_out_addr == 0 || tx_pending) return false;

    // GIP ACK payload format (9 bytes) — matches what a real Xbox sends:
    //   [0] = 0x00
    //   [1] = command being ACKed
    //   [2] = 0x20 (internal flag constant)
    //   [3..8] = 0x00 (padding / chunk progress for chunked ACKs)
    uint8_t ack_payload[9] = {0};
    ack_payload[0] = 0x00;
    ack_payload[1] = acked_cmd;
    ack_payload[2] = GIP_HOST_FLAG_INTERNAL;  // 0x20

    // Build ACK packet — seq must match the acked packet's seq
    gip_tx_buf[0] = GIP_HOST_CMD_ACK;
    gip_tx_buf[1] = GIP_HOST_FLAG_INTERNAL;  // 0x20
    gip_tx_buf[2] = acked_seq;
    gip_tx_buf[3] = sizeof(ack_payload);  // 9

    memcpy(&gip_tx_buf[4], ack_payload, sizeof(ack_payload));

    tuh_xfer_t xfer = {
        .daddr       = auth_dev_addr,
        .ep_addr     = ep_out_addr,
        .buffer      = gip_tx_buf,
        .buflen      = 4 + sizeof(ack_payload),  // 13 bytes
        .complete_cb = on_tx_complete,
        .user_data   = 0,
    };

    tx_pending = true;
    if (!tuh_edpt_xfer(&xfer)) {
        tx_pending = false;
        return false;
    }

    printf("[AUTH] ACK sent for cmd=0x%02X seq=%d\n", acked_cmd, acked_seq);
    return true;
}

// Chunked ACK: includes received/remaining byte counts in payload
// Matches Xbox ACK format: [0x00, cmd, 0x20, rcv_lo, rcv_hi, 0, 0, rem_lo, rem_hi]
static bool send_gip_chunked_ack_now(uint8_t acked_cmd, uint8_t acked_seq,
                                      uint16_t received, uint16_t remaining) {
    if (!endpoints_open || ep_out_addr == 0 || tx_pending) return false;

    uint8_t ack_payload[9] = {0};
    ack_payload[0] = 0x00;
    ack_payload[1] = acked_cmd;
    ack_payload[2] = GIP_HOST_FLAG_INTERNAL;
    ack_payload[3] = (uint8_t)(received & 0xFF);
    ack_payload[4] = (uint8_t)(received >> 8);
    // [5..6] = 0x00
    ack_payload[7] = (uint8_t)(remaining & 0xFF);
    ack_payload[8] = (uint8_t)(remaining >> 8);

    gip_tx_buf[0] = GIP_HOST_CMD_ACK;
    gip_tx_buf[1] = GIP_HOST_FLAG_INTERNAL;
    gip_tx_buf[2] = acked_seq;
    gip_tx_buf[3] = sizeof(ack_payload);  // 9

    memcpy(&gip_tx_buf[4], ack_payload, sizeof(ack_payload));

    tuh_xfer_t xfer = {
        .daddr       = auth_dev_addr,
        .ep_addr     = ep_out_addr,
        .buffer      = gip_tx_buf,
        .buflen      = 4 + sizeof(ack_payload),
        .complete_cb = on_tx_complete,
        .user_data   = 0,
    };

    tx_pending = true;
    if (!tuh_edpt_xfer(&xfer)) {
        tx_pending = false;
        return false;
    }

    printf("[AUTH] Chunked ACK sent: cmd=0x%02X seq=%d rcv=%d rem=%d\n",
           acked_cmd, acked_seq, received, remaining);
    return true;
}

// ─── ACK Queue (handles both simple and chunked ACKs) ───

static void queue_ack(uint8_t cmd, uint8_t flags, uint8_t seq,
                       uint16_t received, uint16_t remaining) {
    for (int i = 0; i < ACK_QUEUE_SIZE; i++) {
        uint8_t idx = (ack_queue_head + i) % ACK_QUEUE_SIZE;
        if (!ack_queue[idx].valid) {
            ack_queue[idx].cmd = cmd;
            ack_queue[idx].flags = flags;
            ack_queue[idx].seq = seq;
            ack_queue[idx].received = received;
            ack_queue[idx].remaining = remaining;
            ack_queue[idx].valid = true;
            printf("[AUTH] ACK queued: cmd=0x%02X seq=%d (tx busy)\n", cmd, seq);
            return;
        }
    }
    printf("[AUTH] ACK queue full! Dropped cmd=0x%02X seq=%d\n", cmd, seq);
}

// Simple ACK — queue if tx busy
static void send_gip_ack(uint8_t acked_cmd, uint8_t acked_flags, uint8_t acked_seq) {
    if (send_gip_ack_now(acked_cmd, acked_flags, acked_seq)) return;
    queue_ack(acked_cmd, acked_flags, acked_seq, 0, 0);
}

// Chunked ACK — queue if tx busy
static void send_gip_chunked_ack(uint8_t acked_cmd, uint8_t acked_seq,
                                   uint16_t received, uint16_t remaining) {
    if (send_gip_chunked_ack_now(acked_cmd, acked_seq, received, remaining)) return;
    queue_ack(acked_cmd, 0, acked_seq, received, remaining);
}

// Drain pending ACKs — called from auth_host_task when TX is free
static void drain_ack_queue(void) {
    for (int i = 0; i < ACK_QUEUE_SIZE; i++) {
        uint8_t idx = (ack_queue_head + i) % ACK_QUEUE_SIZE;
        if (ack_queue[idx].valid) {
            bool sent;
            if (ack_queue[idx].received > 0) {
                sent = send_gip_chunked_ack_now(
                    ack_queue[idx].cmd, ack_queue[idx].seq,
                    ack_queue[idx].received, ack_queue[idx].remaining);
            } else {
                sent = send_gip_ack_now(
                    ack_queue[idx].cmd, ack_queue[idx].flags,
                    ack_queue[idx].seq);
            }
            if (sent) {
                ack_queue[idx].valid = false;
                ack_queue_head = (idx + 1) % ACK_QUEUE_SIZE;
                return;  // One per iteration — don't monopolize TX
            }
            return;  // TX still busy, try next iteration
        }
    }
}

// ─── USB Transfer Callbacks ───

static void on_rx_complete(tuh_xfer_t *xfer) {
    if (xfer->result == XFER_RESULT_SUCCESS) {
        rx_actual_len = xfer->actual_len;
        rx_pending = true;
    } else {
        rx_actual_len = 0;
        rx_pending = false;
        // Log failure and schedule retry
        static uint32_t rx_fail_count = 0;
        rx_fail_count++;
        if (rx_fail_count <= 5 || (rx_fail_count % 1000) == 0) {
            printf("[AUTH] RX failed (result=%d, count=%lu)\n",
                   xfer->result, (unsigned long)rx_fail_count);
        }
        rx_needs_retry = true;
    }
}

static void on_tx_complete(tuh_xfer_t *xfer) {
    (void)xfer;
    tx_pending = false;
}

// Find our IN/OUT endpoints in the PIO-USB pool (cached after first lookup)
static endpoint_t *pio_ep_in = NULL;
static endpoint_t *pio_ep_out = NULL;

static endpoint_t *find_pio_ep_in(void) {
    if (pio_ep_in) return pio_ep_in;
    for (int i = 0; i < PIO_USB_EP_POOL_CNT; i++) {
        endpoint_t *ep = PIO_USB_ENDPOINT(i);
        if (ep->dev_addr == auth_dev_addr && ep->ep_num == ep_in_addr && ep->size > 0) {
            pio_ep_in = ep;
            return ep;
        }
    }
    return NULL;
}

static endpoint_t *find_pio_ep_out(void) {
    if (pio_ep_out) return pio_ep_out;
    for (int i = 0; i < PIO_USB_EP_POOL_CNT; i++) {
        endpoint_t *ep = PIO_USB_ENDPOINT(i);
        if (ep->dev_addr == auth_dev_addr && ep->ep_num == ep_out_addr && ep->size > 0) {
            pio_ep_out = ep;
            return ep;
        }
    }
    return NULL;
}

static bool start_rx(void) {
    if (!endpoints_open || ep_in_addr == 0) return false;

    endpoint_t *ep = find_pio_ep_in();
    if (!ep || ep->has_transfer) return false;

    // Submit transfer directly via PIO-USB, bypassing TinyUSB callback
    bool ok = pio_usb_ll_transfer_start(ep, gip_rx_buf, sizeof(gip_rx_buf));
    static bool first_rx = true;
    if (first_rx) {
        printf("[AUTH] start_rx (direct PIO): ep=0x%02X result=%d\n",
               ep_in_addr, ok);
        first_rx = false;
    }
    return ok;
}

static bool send_gip(uint8_t cmd, uint8_t flags,
                      const uint8_t *payload, uint16_t payload_len) {
    if (!endpoints_open || ep_out_addr == 0 || tx_pending) return false;

    uint8_t total = gip_build(gip_tx_buf, cmd, flags, payload, payload_len);

    tuh_xfer_t xfer = {
        .daddr       = auth_dev_addr,
        .ep_addr     = ep_out_addr,
        .buffer      = gip_tx_buf,
        .buflen      = total,
        .complete_cb = on_tx_complete,
        .user_data   = 0,
    };

    tx_pending = true;
    if (!tuh_edpt_xfer(&xfer)) {
        tx_pending = false;
        return false;
    }
    return true;
}

// ─── Config Descriptor Parsing ───

static void on_config_desc_received(tuh_xfer_t *xfer) {
    if (xfer->result != XFER_RESULT_SUCCESS || xfer->actual_len < 9) {
        printf("[AUTH] Failed to get config descriptor\n");
        gip_state = GIP_HOST_IDLE;
        return;
    }

    uint16_t total_len = xfer->actual_len;
    uint8_t *p = config_desc_buf;
    uint8_t *end = p + total_len;

    bool found_iface = false;

    while (p < end) {
        uint8_t desc_len = p[0];
        uint8_t desc_type = p[1];

        if (desc_len == 0) break;

        if (desc_type == TUSB_DESC_INTERFACE && (p + 9) <= end) {
            uint8_t iface_class = p[5];
            uint8_t iface_subclass = p[6];
            uint8_t iface_protocol = p[7];

            if (iface_class == GIP_IFACE_CLASS &&
                iface_subclass == GIP_IFACE_SUBCLASS &&
                iface_protocol == GIP_IFACE_PROTOCOL) {
                found_iface = true;
                printf("[AUTH] Found GIP interface: class=%02X/%02X/%02X\n",
                       iface_class, iface_subclass, iface_protocol);
            } else {
                found_iface = false;
            }
        }

        if (desc_type == TUSB_DESC_ENDPOINT && found_iface && (p + 7) <= end) {
            tusb_desc_endpoint_t *ep = (tusb_desc_endpoint_t *)p;
            uint8_t ep_addr = ep->bEndpointAddress;

            if (ep_addr & 0x80) {  // IN endpoint
                if (ep_in_addr == 0) {
                    ep_in_addr = ep_addr;
                    tuh_edpt_open(auth_dev_addr, ep);
                    printf("[AUTH] Opened IN endpoint 0x%02X [ACTIVE]\n", ep_addr);
                }
                // Skip duplicates — opening same EP twice corrupts TinyUSB tracking
            } else {  // OUT endpoint
                if (ep_out_addr == 0) {
                    ep_out_addr = ep_addr;
                    tuh_edpt_open(auth_dev_addr, ep);
                    printf("[AUTH] Opened OUT endpoint 0x%02X [ACTIVE]\n", ep_addr);
                }
            }
        }

        p += desc_len;
    }

    if (ep_in_addr && ep_out_addr) {
        endpoints_open = true;
        gip_state = GIP_HOST_READY;
        auth_mount_time = to_ms_since_boot(get_absolute_time());
        // Don't call start_rx() here — we're inside a TinyUSB callback.
        // PIO-USB may not handle new transfers submitted from callbacks.
        // Defer to auth_host_task() main loop.
        rx_needs_start = true;
        printf("[AUTH] Endpoints ready — RX deferred to main loop\n");
    } else {
        printf("[AUTH] ERROR: GIP endpoints not found\n");
        gip_state = GIP_HOST_IDLE;
    }
}

// ─── GIP Protocol Handlers ───

static bool powerA_initialized = false;

static void handle_powerA_announce(const uint8_t *payload, uint16_t len) {
    (void)payload; (void)len;

    if (powerA_initialized) {
        return;
    }
    powerA_initialized = true;

    // Send IDENTIFY request first — controller needs this handshake
    // completed before its auth chip activates. Power ON comes after.
    printf("[AUTH] PowerA ANNOUNCE — sending IDENTIFY request\n");
    send_gip(GIP_HOST_CMD_IDENTIFY, GIP_HOST_FLAG_INTERNAL, NULL, 0);

    identify_rx_offset = 0;
    identify_complete = false;
    identify_total_raw = 0;
    identify_received_raw = 0;
    gip_state = GIP_HOST_WAIT_IDENTIFY;
}

static void handle_identify_chunk(const uint8_t *payload, uint16_t payload_len,
                                    uint8_t flags, uint8_t seq) {
    bool chunked = (flags & GIP_HOST_FLAG_CHUNKED) != 0;
    bool chunk_start = (flags & GIP_HOST_FLAG_CHUNK_START) != 0;
    bool needs_ack = (flags & GIP_HOST_FLAG_ACK) != 0;

    if (!chunked) {
        // Single-packet IDENTIFY (unlikely for controllers)
        if (payload_len <= sizeof(identify_rx_buf)) {
            memcpy(identify_rx_buf, payload, payload_len);
            identify_rx_offset = payload_len;
        }
        identify_complete = true;
        printf("[AUTH] IDENTIFY single packet (%d bytes)\n", payload_len);
        if (needs_ack) send_gip_ack(GIP_HOST_CMD_IDENTIFY, flags, seq);
        return;
    }

    if (chunk_start) {
        // First chunk: payload[0..1] = XGIP hashed total length (little-endian)
        uint16_t hashed_total = payload[0] | ((uint16_t)payload[1] << 8);
        identify_total_raw = xgip_unhash_length(hashed_total);
        identify_received_raw = 0;
        identify_rx_offset = 0;
        identify_complete = false;
        printf("[AUTH] IDENTIFY started: hash=0x%04X raw_total=%d\n",
               hashed_total, identify_total_raw);
    }

    // Accumulate data (skip 2-byte chunk offset prefix)
    if (payload_len > 2) {
        uint16_t data_len = payload_len - 2;
        if (identify_rx_offset + data_len <= sizeof(identify_rx_buf)) {
            memcpy(&identify_rx_buf[identify_rx_offset], &payload[2], data_len);
            identify_rx_offset += data_len;
        }
    }

    // Track cumulative raw payload bytes (full payload_len, not just data)
    identify_received_raw += payload_len;

    // ACK with chunk progress if requested
    if (needs_ack) {
        uint16_t remaining = 0;
        if (identify_total_raw > identify_received_raw)
            remaining = identify_total_raw - identify_received_raw;
        send_gip_chunked_ack(GIP_HOST_CMD_IDENTIFY, seq,
                              identify_received_raw, remaining);
        printf("[AUTH] IDENTIFY chunk: +%d bytes (rcv=%d rem=%d)\n",
               payload_len, identify_received_raw, remaining);
    }

    // Detect completion
    if (identify_total_raw > 0 && identify_received_raw >= identify_total_raw) {
        identify_complete = true;
    } else if (payload_len > 0 && payload_len < GIP_HOST_MAX_CHUNK_DATA && !chunk_start) {
        identify_complete = true;  // Short last chunk
    }

    if (identify_complete) {
        printf("[AUTH] IDENTIFY complete: %d data bytes, %d raw bytes\n",
               identify_rx_offset, identify_received_raw);
    }
}

// Track last auth response seq to de-duplicate controller retries
static uint8_t last_auth_rx_seq = 0;
static bool last_auth_rx_seq_valid = false;

static void handle_powerA_auth_response(const uint8_t *payload, uint16_t len,
                                         uint8_t flags, uint8_t seq) {
    // De-duplicate: controller retries with same seq when it doesn't get our ACK
    if (last_auth_rx_seq_valid && seq == last_auth_rx_seq) {
        return;  // Already relayed this response
    }
    last_auth_rx_seq = seq;
    last_auth_rx_seq_valid = true;

    bool chunked = (flags & GIP_HOST_FLAG_CHUNKED) != 0;
    bool chunk_start = (flags & GIP_HOST_FLAG_CHUNK_START) != 0;

    if (!chunked) {
        if (len <= sizeof(auth_rx_buf)) {
            memcpy(auth_rx_buf, payload, len);
            auth_rx_len = len;
            auth_rx_ready = true;
            printf("[AUTH] Auth response seq=%d (%d bytes, single)\n", seq, len);
        }
        return;
    }

    if (chunk_start) {
        auth_rx_offset = 0;
        auth_rx_len = 0;
        auth_rx_ready = false;
    }

    if (auth_rx_offset + len <= sizeof(auth_rx_buf)) {
        memcpy(&auth_rx_buf[auth_rx_offset], payload, len);
        auth_rx_offset += len;
    }

    uint16_t max_payload_in_packet = 64 - 4;
    if (len < max_payload_in_packet) {
        auth_rx_len = auth_rx_offset;
        auth_rx_ready = true;
        printf("[AUTH] Auth response reassembled (%d bytes, chunked)\n", auth_rx_len);
    }
}

static void process_gip_packet(const uint8_t *pkt, uint16_t pkt_len) {
    uint8_t cmd, flags, seq;
    const uint8_t *payload;
    uint16_t payload_len;

    if (!gip_parse(pkt, pkt_len, &cmd, &flags, &seq, &payload, &payload_len)) {
        return;
    }

    bool needs_ack = (flags & GIP_HOST_FLAG_ACK) != 0;

    switch (cmd) {
        case GIP_HOST_CMD_ANNOUNCE:
            handle_powerA_announce(payload, payload_len);
            break;
        case GIP_HOST_CMD_AUTHENTICATE:
            handle_powerA_auth_response(payload, payload_len, flags, seq);
            // Don't ACK auth responses — our ACK format isn't accepted by
            // the controller (same issue as IDENTIFY chunks). Without ACK,
            // controller retries ~3x then stops, which is fine.
            break;
        case GIP_HOST_CMD_IDENTIFY:
            handle_identify_chunk(payload, payload_len, flags, seq);
            break;
        case GIP_HOST_CMD_STATUS:
            if (needs_ack) send_gip_ack(cmd, flags, seq);
            break;
        case GIP_HOST_CMD_INPUT:
            break;
        default:
            printf("[AUTH] Controller cmd=0x%02X (%d bytes)\n", cmd, payload_len);
            if (needs_ack) send_gip_ack(cmd, flags, seq);
            break;
    }
}

// ─── Auth Challenge TX (chunked GIP 0x06 to PowerA) ───

static void send_next_auth_chunk(void) {
    if (!auth_tx_queued || tx_pending) return;

    uint16_t remaining = auth_tx_len - auth_tx_offset;
    if (remaining == 0) {
        auth_tx_queued = false;
        return;
    }

    uint16_t max_chunk = 60;
    uint16_t chunk_len = remaining < max_chunk ? remaining : max_chunk;
    bool first = (auth_tx_offset == 0);
    bool fits_single = (auth_tx_len <= max_chunk);

    // Xbox sends auth challenges with INTERNAL flag (0x20) always set
    uint8_t flags = GIP_HOST_FLAG_INTERNAL;
    if (!fits_single) {
        if (first) {
            flags |= GIP_HOST_FLAG_CHUNK_START | GIP_HOST_FLAG_CHUNKED;
        } else if (remaining > max_chunk) {
            flags |= GIP_HOST_FLAG_CHUNKED;
        }
    }

    if (send_gip(GIP_HOST_CMD_AUTHENTICATE, flags,
                  &auth_tx_buf[auth_tx_offset], chunk_len)) {
        auth_tx_offset += chunk_len;
        if (auth_tx_offset >= auth_tx_len) {
            printf("[AUTH] Challenge sent to PowerA (%d bytes)\n", auth_tx_len);
            auth_tx_queued = false;
        }
    }
}

// ─── TinyUSB Host Callbacks (called from tuh_task on Core 1) ───

void tuh_mount_cb(uint8_t dev_addr) {
    uint16_t vid, pid;
    tuh_vid_pid_get(dev_addr, &vid, &pid);
    printf("[AUTH] Device mounted: addr=%d VID=%04X PID=%04X\n", dev_addr, vid, pid);

    auth_dev_addr = dev_addr;
    auth_connected = true;
    gip_state = GIP_HOST_OPENING_ENDPOINTS;
    memset(gip_seq_counters, 0, sizeof(gip_seq_counters));

    tuh_descriptor_get_configuration(dev_addr, 0, config_desc_buf,
                                      sizeof(config_desc_buf),
                                      on_config_desc_received, 0);
}

void tuh_umount_cb(uint8_t dev_addr) {
    if (dev_addr == auth_dev_addr) {
        printf("[AUTH] PowerA disconnected\n");
        auth_connected = false;
        auth_dev_addr = 0;
        ep_in_addr = 0;
        ep_out_addr = 0;
        endpoints_open = false;
        gip_state = GIP_HOST_IDLE;
        auth_tx_queued = false;
        auth_rx_ready = false;
        rx_pending = false;
        rx_needs_start = false;
        tx_pending = false;
        auth_relay_started = false;
        powerA_initialized = false;
        identify_rx_offset = 0;
        identify_complete = false;
        identify_total_raw = 0;
        identify_received_raw = 0;
        last_auth_rx_seq = 0;
        last_auth_rx_seq_valid = false;
        memset(ack_queue, 0, sizeof(ack_queue));
        ack_queue_head = 0;
        pio_ep_in = NULL;
        pio_ep_out = NULL;
    }
}

// ─── Public API ───

bool auth_host_init(void) {
    // Configure PIO-USB for TinyUSB host on port 1 (PIO0)
    pio_usb_configuration_t pio_cfg = PIO_USB_DEFAULT_CONFIG;
    pio_cfg.pin_dp = AUTH_PIO_USB_DP_PIN;
    pio_cfg.pinout = PIO_USB_PINOUT_DPDM;

    printf("[AUTH] Configuring PIO-USB: DP=GPIO%d, PIO=%d, SM_TX=%d, SM_RX=%d\n",
           pio_cfg.pin_dp, pio_cfg.pio_tx_num, pio_cfg.sm_tx, pio_cfg.sm_rx);

    bool cfg_ok = tuh_configure(1, TUH_CFGID_RPI_PIO_USB_CONFIGURATION, &pio_cfg);
    bool init_ok = tuh_init(1);

    printf("[AUTH] PIO-USB host initialized on GPIO %d/%d (cfg=%d init=%d)\n",
           AUTH_PIO_USB_DP_PIN, AUTH_PIO_USB_DM_PIN, cfg_ok, init_ok);
    return init_ok;
}

void auth_host_task(void) {
    tuh_task();

    // Periodic heartbeat — confirm Core 1 PIO-USB is running
    static uint32_t last_heartbeat = 0;
    static uint32_t task_count = 0;
    task_count++;
    uint32_t now = to_ms_since_boot(get_absolute_time());
    if (now - last_heartbeat >= 10000) {  // Every 10s
        printf("[AUTH-HB] Core1 alive: %lu tasks, connected=%d, endpoints=%d\n",
               (unsigned long)task_count, auth_connected, endpoints_open);
        // Dump PIO-USB root port and endpoint pool state
        for (int ri = 0; ri < PIO_USB_ROOT_PORT_CNT; ri++) {
            root_port_t *root = PIO_USB_ROOT_PORT(ri);
            if (root->initialized) {
                printf("[AUTH-HB] Root[%d]: init=%d conn=%d susp=%d ints=0x%08lX\n",
                       ri, root->initialized, root->connected, root->suspended,
                       (unsigned long)root->ints);
            }
        }
        for (int ei = 0; ei < PIO_USB_EP_POOL_CNT; ei++) {
            endpoint_t *ep = PIO_USB_ENDPOINT(ei);
            if (ep->size > 0) {
                printf("[AUTH-HB] EP[%d]: addr=%d ep=0x%02X sz=%d attr=%d "
                       "int=%d has_xfer=%d started=%d aborted=%d stalled=%d "
                       "is_tx=%d root=%d\n",
                       ei, ep->dev_addr, ep->ep_num, ep->size, ep->attr,
                       ep->interval, ep->has_transfer, ep->transfer_started,
                       ep->transfer_aborted, ep->stalled, ep->is_tx, ep->root_idx);
            }
        }
        last_heartbeat = now;
    }

    if (!auth_connected || !endpoints_open) return;

    // Deferred start_rx — submitted from main loop, not from TinyUSB callback
    if (rx_needs_start && !rx_pending) {
        rx_needs_start = false;
        bool ok = start_rx();
        printf("[AUTH] Deferred start_rx: ep=0x%02X result=%d\n", ep_in_addr, ok);
    }

    // Wait for init delay before processing GIP packets from controller
    if (!auth_relay_started) {
        uint32_t now = to_ms_since_boot(get_absolute_time());
        if (now - auth_mount_time < AUTH_INIT_DELAY_MS) return;
        auth_relay_started = true;
        // IDENTIFY request is now sent in handle_powerA_announce() after
        // we receive the controller's ANNOUNCE — the correct GIP sequence.
        printf("[AUTH] Auth relay ACTIVE (after %lums delay)\n",
               (unsigned long)(now - auth_mount_time));
    }

    // Retry RX if previous transfer failed
    if (rx_needs_retry && !rx_pending) {
        rx_needs_retry = false;
        start_rx();
    }

    // Direct PIO-USB endpoint polling — bypass TinyUSB callback entirely
    {
        endpoint_t *ep = find_pio_ep_in();
        if (ep && !ep->has_transfer && ep->actual_len > 0) {
            // Transfer completed with data!
            uint16_t len = ep->actual_len;
            printf("[AUTH-RX] %d bytes:", len);
            for (uint16_t i = 0; i < len && i < 16; i++)
                printf(" %02X", gip_rx_buf[i]);
            printf("\n");
            process_gip_packet(gip_rx_buf, len);
            // Resubmit
            start_rx();
        } else if (ep && !ep->has_transfer && !ep->stalled) {
            // Transfer completed with no data (NAK timeout or 0-length) — resubmit
            start_rx();
        }
    }

    // Poll OUT endpoint to detect TX completion — TinyUSB on_tx_complete
    // callback doesn't fire reliably with PIO-USB dual-stack mode
    if (tx_pending) {
        endpoint_t *ep_out = find_pio_ep_out();
        if (ep_out && !ep_out->has_transfer) {
            tx_pending = false;
        }
    }

    // Drain queued ACKs when TX is free
    if (!tx_pending) {
        drain_ack_queue();
    }

    // After IDENTIFY completes, send Power ON to activate auth chip
    if (gip_state == GIP_HOST_WAIT_IDENTIFY && identify_complete && !tx_pending) {
        printf("[AUTH] IDENTIFY done — sending Power ON\n");
        uint8_t power_payload[] = { GIP_HOST_STATE_ON };
        send_gip(GIP_HOST_CMD_POWER, GIP_HOST_FLAG_INTERNAL,
                  power_payload, sizeof(power_payload));
        gip_state = GIP_HOST_READY;  // Transition even if send queued
        printf("[AUTH] GIP host READY — auth relay active\n");
    }

    // Check for new challenge from Core 0
    if (challenge_ready && gip_state == GIP_HOST_READY && !auth_tx_queued) {
        __dmb();  // Acquire barrier — ensure we see Core 0's data writes
        memcpy(auth_tx_buf, challenge_buf, challenge_len);
        auth_tx_len = challenge_len;
        auth_tx_offset = 0;
        auth_tx_queued = true;
        challenge_ready = false;
        printf("[AUTH] Picked up %d-byte challenge from Core 0\n", auth_tx_len);
    }

    // Send queued auth challenge chunks
    if (auth_tx_queued && !tx_pending) {
        send_next_auth_chunk();
    }

    // If auth response ready, publish to Core 0
    if (auth_rx_ready && !response_ready) {
        memcpy(response_buf, auth_rx_buf, auth_rx_len);
        response_len = auth_rx_len;
        __dmb();  // Ensure data visible before flag
        response_ready = true;
        auth_rx_ready = false;
        printf("[AUTH] Response published for Core 0 (%d bytes)\n", response_len);
    }
}

bool auth_host_connected(void) {
    return auth_connected && gip_state == GIP_HOST_READY;
}

// Called from Core 0 — queue challenge for Core 1
bool auth_host_send_challenge(const uint8_t *challenge, uint16_t len) {
    if (challenge_ready) return false;  // Previous challenge not consumed yet
    if (len > AUTH_MAX_PAYLOAD) return false;

    memcpy(challenge_buf, challenge, len);
    challenge_len = len;
    __dmb();  // Ensure data visible before flag
    challenge_ready = true;
    return true;
}

// Called from Core 0 — retrieve response from Core 1
uint16_t auth_host_get_response(uint8_t *response, uint16_t max_len) {
    if (!response_ready) return 0;
    __dmb();  // Acquire barrier — ensure we see Core 1's data writes

    uint16_t copy = response_len;
    if (copy > max_len) copy = max_len;
    memcpy(response, response_buf, copy);

    response_ready = false;
    response_len = 0;
    return copy;
}
