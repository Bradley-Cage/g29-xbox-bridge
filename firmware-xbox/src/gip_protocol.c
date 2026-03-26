/**
 * gip_protocol.c — Xbox GIP Protocol State Machine
 *
 * Handles GIP packet framing, command dispatch, sequence tracking,
 * and keepalive management over TinyUSB vendor endpoints.
 *
 * GIP packet format:
 *   Byte 0: Command ID (0x01-0x60)
 *   Byte 1: Client(3:0) | Flags(7:4) — ACK=0x10, Internal=0x20,
 *           ChunkStart=0x40, Chunked=0x80
 *   Byte 2: Sequence number (per-command, 1-255, wraps to 1)
 *   Byte 3+: LEB128-encoded payload length (1-2 bytes typically)
 *   Remaining: Payload
 */

#include "gip_protocol.h"
#include "gip_wheel.h"
#include "auth_host.h"
#include "spi_protocol.h"
#include "tusb.h"
#include "pico/unique_id.h"

#include <string.h>
#include <stdio.h>

#define ANNOUNCE_INTERVAL_MS  500  // Send ANNOUNCE every 500ms until Xbox responds

// ─── State ───

static gip_state_t state;
static gip_chunked_tx_t chunked_tx;  // Active chunked transfer
static uint8_t rx_buf[64];   // Incoming GIP packets from Xbox
static uint8_t tx_buf[64];   // Outgoing GIP packets to Xbox

// Debug: last command received from Xbox
static uint8_t last_rx_cmd = 0;

// Auth relay buffers
static uint8_t auth_challenge_buf[SPI_AUTH_MAX_PAYLOAD];
static uint16_t auth_challenge_len = 0;
static bool auth_challenge_pending = false;

// External: metadata blob
extern const uint8_t *gip_get_metadata_blob(void);
extern uint16_t gip_get_metadata_len(void);

// ─── LEB128 encoding ───

static uint8_t leb128_encode(uint16_t value, uint8_t *out) {
    if (value < 128) {
        out[0] = (uint8_t)value;
        return 1;
    }
    out[0] = (uint8_t)(value & 0x7F) | 0x80;
    out[1] = (uint8_t)(value >> 7);
    return 2;
}

static uint16_t leb128_decode(const uint8_t *data, uint8_t *bytes_consumed) {
    if (!(data[0] & 0x80)) {
        *bytes_consumed = 1;
        return data[0];
    }
    *bytes_consumed = 2;
    return (data[0] & 0x7F) | ((uint16_t)data[1] << 7);
}

// ─── XGIP Chunk Length Hashing ───
// GP2040-CE uses a "hashed" length that inflates values at 0x100 boundaries.
// This must match exactly or Xbox rejects the chunked transfer.

static uint16_t xgip_compute_chunk_length(uint16_t data_len) {
    uint16_t i = data_len;
    uint16_t j = 0;
    do {
        if (i < GIP_MAX_CHUNK_DATA) {
            if ((j / 0x100) != ((j + i) / 0x100))
                j = j + (i | 0x80);
            else
                j = j + i;
            i = 0;
        } else {
            if ((j + GIP_MAX_CHUNK_DATA > 0x80) && (j + GIP_MAX_CHUNK_DATA < 0x100))
                j = j + GIP_MAX_CHUNK_DATA + 0x100;
            else if ((j / 0x100) != ((j + GIP_MAX_CHUNK_DATA) / 0x100))
                j = j + (GIP_MAX_CHUNK_DATA | 0x80);
            else
                j = j + GIP_MAX_CHUNK_DATA;
            i = i - GIP_MAX_CHUNK_DATA;
        }
    } while (i != 0);
    return j;
}

// Incrementally update the "hashed" chunk-sent counter after sending data_sent bytes
static uint16_t xgip_update_chunk_sent(uint16_t current, uint16_t data_sent) {
    if ((current / 0x100) != ((current + data_sent) / 0x100))
        return current + (data_sent | 0x80);
    if ((current + data_sent > 0x80) && (current + data_sent < 0x100))
        return current + data_sent + 0x100;
    return current + data_sent;
}

// Write 2-byte chunk offset value (GP2040-CE's unusual byte ordering)
static void xgip_write_chunk_value(uint8_t *buf, uint16_t value) {
    if (value < 0x100) {
        buf[0] = 0x00;
        buf[1] = (uint8_t)value;
    } else {
        buf[0] = value & 0xFF;
        buf[1] = (value >> 8) & 0xFF;
    }
}

// ─── Packet Building ───

static uint8_t gip_build_header(uint8_t *buf, uint8_t cmd, uint8_t flags,
                                 uint16_t payload_len) {
    buf[0] = cmd;
    buf[1] = flags;
    // GP2040-CE uses sequence=1 (fixed) for ANNOUNCE, incrementing for others
    buf[2] = (cmd == GIP_CMD_ANNOUNCE) ? 1 : gip_next_seq(cmd);
    buf[3] = (uint8_t)payload_len;  // Simple length byte (not LEB128 for GIP)
    return 4;
}

static bool gip_send(uint8_t cmd, uint8_t flags,
                      const uint8_t *payload, uint16_t payload_len) {
    if (!tud_vendor_mounted()) return false;

    uint8_t header_len = gip_build_header(tx_buf, cmd, flags, payload_len);

    if (payload_len > 0 && payload != NULL) {
        if (header_len + payload_len > sizeof(tx_buf)) return false;
        memcpy(&tx_buf[header_len], payload, payload_len);
    }

    uint32_t written = tud_vendor_write(tx_buf, header_len + payload_len);
    tud_vendor_write_flush();
    return written == (header_len + payload_len);
}

// ─── Chunked Transfer State Machine ───

static void chunked_tx_start(uint8_t cmd, uint8_t seq,
                              const uint8_t *data, uint16_t data_len) {
    chunked_tx.active = true;
    chunked_tx.command = cmd;
    chunked_tx.sequence = seq;
    chunked_tx.data = data;
    chunked_tx.data_len = data_len;
    chunked_tx.total_sent = 0;
    chunked_tx.total_chunk_len = xgip_compute_chunk_length(data_len);
    chunked_tx.total_chunk_sent = 0;
    chunked_tx.chunks_sent = 0;
    chunked_tx.last_chunk_ms = 0;
    chunked_tx.waiting_ack = false;

    printf("[GIP] Chunked TX started: cmd=0x%02X len=%d chunk_hash=0x%04X\n",
           cmd, data_len, chunked_tx.total_chunk_len);
}

// Send next chunk. Called from gip_task() with timing.
static bool chunked_tx_send_next(void) {
    if (!chunked_tx.active || !tud_vendor_mounted()) return false;

    // All data sent? Send end-of-chunk packet
    if (chunked_tx.total_sent >= chunked_tx.data_len) {
        tx_buf[0] = chunked_tx.command;
        tx_buf[1] = GIP_FLAG_CHUNKED | GIP_FLAG_INTERNAL;  // chunked + internal
        tx_buf[2] = chunked_tx.sequence;
        tx_buf[3] = 0x00;  // length=0 signals end
        xgip_write_chunk_value(&tx_buf[4], chunked_tx.total_chunk_len);

        tud_vendor_write(tx_buf, 6);
        tud_vendor_write_flush();

        printf("[GIP] Chunked TX complete: %d bytes in %d chunks\n",
               chunked_tx.data_len, chunked_tx.chunks_sent);
        chunked_tx.active = false;
        return true;
    }

    // Calculate this chunk's data size
    uint16_t remaining = chunked_tx.data_len - chunked_tx.total_sent;
    uint16_t data_to_send = (remaining < GIP_MAX_CHUNK_DATA) ? remaining : GIP_MAX_CHUNK_DATA;
    bool is_first = (chunked_tx.chunks_sent == 0);
    bool is_last_data = (remaining <= GIP_MAX_CHUNK_DATA);

    // Build flags
    uint8_t flags = GIP_FLAG_CHUNKED;
    if (is_first) flags |= GIP_FLAG_CHUNK_START;
    flags |= GIP_FLAG_INTERNAL;

    // needsAck on: first chunk, every 5th, last data chunk
    if (is_first || ((chunked_tx.chunks_sent + 1) % 5 == 0) || is_last_data) {
        flags |= GIP_FLAG_ACK;
    }

    // Build header
    tx_buf[0] = chunked_tx.command;
    tx_buf[1] = flags;
    tx_buf[2] = chunked_tx.sequence;

    // Length byte with |0x80 encoding per GP2040-CE
    if (chunked_tx.chunks_sent > 0 && chunked_tx.total_chunk_sent < 0x100) {
        tx_buf[3] = data_to_send | 0x80;
    } else if (is_first && chunked_tx.data_len > GIP_MAX_CHUNK_DATA &&
               chunked_tx.data_len < 0x80) {
        tx_buf[3] = data_to_send | 0x80;
    } else {
        tx_buf[3] = data_to_send;
    }

    // 2-byte chunk offset field
    if (is_first) {
        xgip_write_chunk_value(&tx_buf[4], chunked_tx.total_chunk_len);
    } else {
        xgip_write_chunk_value(&tx_buf[4], chunked_tx.total_chunk_sent);
    }

    // Copy payload data
    memcpy(&tx_buf[6], &chunked_tx.data[chunked_tx.total_sent], data_to_send);

    uint32_t total = 6 + data_to_send;
    uint32_t written = tud_vendor_write(tx_buf, total);
    tud_vendor_write_flush();

    printf("[GIP] Chunk %d: %d bytes, flags=0x%02X, written=%lu\n",
           chunked_tx.chunks_sent, data_to_send, flags, (unsigned long)written);

    // Update state
    chunked_tx.total_sent += data_to_send;
    chunked_tx.total_chunk_sent = xgip_update_chunk_sent(
        chunked_tx.total_chunk_sent, data_to_send);
    chunked_tx.chunks_sent++;
    chunked_tx.last_chunk_ms = to_ms_since_boot(get_absolute_time());

    // If this chunk needs ACK, block until received
    if (flags & GIP_FLAG_ACK) {
        chunked_tx.waiting_ack = true;
        chunked_tx.ack_wait_start = chunked_tx.last_chunk_ms;
    }

    return true;
}

// ─── Protocol Handlers ───

static void handle_hello_request(void) {
    // GIP Device Arrival (0x02): use GP2040-CE's proven 28-byte payload
    // Our previous MS-GIPUSB spec-based format was rejected by Xbox.
    // GP2040-CE's hardcoded blob is what actually works.
    static uint8_t hello_payload[] = {
        0x00, 0x2A, 0x00, 0xFF, 0xFF, 0xFF, 0x00, 0x00,
        0xDF, 0x33, 0x14, 0x00, 0x01, 0x00, 0x01, 0x00,
        0x17, 0x01, 0x02, 0x00, 0x01, 0x00, 0x01, 0x00,
        0x01, 0x00, 0x01, 0x00
    };

    // Overwrite bytes [3..5] with low 3 bytes of uptime (as GP2040-CE does)
    uint32_t now = to_ms_since_boot(get_absolute_time());
    memcpy(&hello_payload[3], &now, 3);

    gip_send(GIP_CMD_ANNOUNCE, GIP_FLAG_INTERNAL, hello_payload, sizeof(hello_payload));
    state.announce_count++;
    printf("[GIP] ANNOUNCE #%d sent (%d bytes)\n", state.announce_count,
           (int)sizeof(hello_payload));
}

static void handle_metadata_request(uint8_t request_seq,
                                     const uint8_t *payload, uint16_t len) {
    (void)payload; (void)len;
    printf("[GIP] Received IDENTIFY request seq=%d (after %d ANNOUNCEs)\n",
           request_seq, state.announce_count);

    // Only respond once — GP2040-CE transitions to SETUP_AUTH after first send
    if (state.metadata_sent || chunked_tx.active) {
        printf("[GIP] Metadata already sent, ignoring IDENTIFY seq=%d\n", request_seq);
        return;
    }

    const uint8_t *blob = gip_get_metadata_blob();
    uint16_t blob_len = gip_get_metadata_len();

    if (blob_len <= GIP_MAX_CHUNK_DATA) {
        // Fits in a single non-chunked packet
        gip_send(GIP_CMD_IDENTIFY, GIP_FLAG_INTERNAL, blob, blob_len);
        state.metadata_sent = true;
        printf("[GIP] Sent metadata (%d bytes, single packet)\n", blob_len);
    } else {
        // Use the REQUEST's sequence number — Xbox matches responses by seq
        chunked_tx_start(GIP_CMD_IDENTIFY, request_seq, blob, blob_len);
    }
}

static void handle_power_state(const uint8_t *payload, uint16_t len) {
    if (len < 1) return;
    uint8_t power_state = payload[0];

    switch (power_state) {
        case GIP_STATE_OFF:
            printf("[GIP] Device state: OFF\n");
            // TODO: signal VM to stop, turn off FFB
            break;
        case GIP_STATE_QUIESCE:
            printf("[GIP] Device state: QUIESCE\n");
            // Clear VM equations, enable return-to-center spring
            // TODO: signal VM via queue
            break;
        case GIP_STATE_ON:
            printf("[GIP] Device state: ON (started)\n");
            state.device_started = true;
            break;
        case 0x07:
            // Power mode 0x07: Device config / initialization mode
            // Xbox sends this after IDENTIFY. Treat as "ready for auth".
            printf("[GIP] Device state: CONFIG (0x07)\n");
            state.device_started = true;
            break;
        default:
            printf("[GIP] Unknown power state: 0x%02X\n", power_state);
            break;
    }
}

// Track last auth challenge seq to de-duplicate Xbox retries
static uint8_t last_auth_challenge_seq = 0;
static bool last_auth_challenge_seq_valid = false;

static void handle_auth(uint8_t seq, const uint8_t *payload, uint16_t len) {
    // De-duplicate: Xbox retries same seq when it doesn't get a response
    if (last_auth_challenge_seq_valid && seq == last_auth_challenge_seq) {
        return;
    }
    last_auth_challenge_seq = seq;
    last_auth_challenge_seq_valid = true;

    printf("[GIP] Auth challenge seq=%d (%d bytes)\n", seq, len);

    if (len > SPI_AUTH_MAX_PAYLOAD) {
        printf("[GIP] ERROR: auth payload too large (%d > %d)\n",
               len, SPI_AUTH_MAX_PAYLOAD);
        return;
    }

    memcpy(auth_challenge_buf, payload, len);
    auth_challenge_len = len;
    auth_challenge_pending = true;
}

static void handle_wheel_request(const uint8_t *payload, uint16_t len) {
    if (len < 3) return;

    uint8_t req_type = payload[0];
    uint16_t req_data = payload[1] | ((uint16_t)payload[2] << 8);

    // Delegate to gip_wheel module for all wheel request subtypes
    gip_wheel_handle_request(req_type, req_data);
}

static void handle_app_memory(const uint8_t *payload, uint16_t len) {
    // Command 0x0B: Set Application Memory (up to 10 values per transfer)
    // Each value: 2 bytes address + 4 bytes value = 6 bytes
    // TODO: Phase 5 — forward to VM via lockfree queue
    printf("[GIP] App memory write (%d bytes)\n", len);
    (void)payload;
}

static void handle_eq_states(const uint8_t *payload, uint16_t len) {
    // Command 0x0C: Set Equation States (9 bytes)
    // TODO: Phase 5 — forward to VM via lockfree queue
    printf("[GIP] Equation states update (%d bytes)\n", len);
    (void)payload;
}

static void handle_eq_set(const uint8_t *payload, uint16_t len) {
    // Command 0x0D: Set Equation bytecode (up to 53 bytes payload)
    // Payload: eq_index(1) + eq_size(1) + eq_offset(1) + bytecode(up to 48)
    if (len < 3) return;
    uint8_t eq_idx = payload[0];
    uint8_t eq_size = payload[1];
    uint8_t eq_offset = payload[2];
    printf("[GIP] Equation %d: size=%d offset=%d data_len=%d\n",
           eq_idx, eq_size, eq_offset, len - 3);
    // TODO: Phase 5 — forward to VM via lockfree queue
}

// ─── Incoming Packet Dispatch ───

static void dispatch_gip_packet(const uint8_t *packet, uint16_t total_len) {
    if (total_len < 4) return;

    uint8_t cmd = packet[0];
    uint8_t flags = packet[1];
    uint8_t seq = packet[2];
    uint8_t len_bytes;
    uint16_t payload_len = leb128_decode(&packet[3], &len_bytes);
    uint8_t header_len = 3 + len_bytes;

    const uint8_t *payload = &packet[header_len];

    last_rx_cmd = cmd;

    // Debug: log every incoming GIP packet
    printf("[GIP-RX] cmd=%02X flags=%02X seq=%d len=%d |",
           cmd, flags, seq, payload_len);
    for (uint16_t i = 0; i < total_len && i < 16; i++) {
        printf(" %02X", packet[i]);
    }
    if (total_len > 16) printf(" ...");
    printf("\n");

    switch (cmd) {
        case GIP_CMD_ACK:
            printf("[GIP] ACK received (seq=%d)\n", seq);
            // Unblock chunked transfer if waiting for ACK
            if (chunked_tx.active && chunked_tx.waiting_ack) {
                chunked_tx.waiting_ack = false;
                printf("[GIP] Chunked TX ACK unblocked\n");
            }
            break;
        case GIP_CMD_IDENTIFY:
            handle_metadata_request(seq, payload, payload_len);
            break;
        case GIP_CMD_POWER:
            handle_power_state(payload, payload_len);
            break;
        case GIP_CMD_AUTHENTICATE:
            handle_auth(seq, payload, payload_len);
            break;
        // Note: GIP_CMD_LED (0x0A) is the same as wheel request command.
        // For wheels, 0x0A is used for wheel-specific requests (angle, power, vibration).
        // LED control is handled via the wheel request handler's subtype dispatch.
        case GIP_CMD_APP_MEMORY:
            handle_app_memory(payload, payload_len);
            break;
        case GIP_CMD_EQ_STATES:
            handle_eq_states(payload, payload_len);
            break;
        case GIP_CMD_EQ_SET:
            handle_eq_set(payload, payload_len);
            break;

        // Wheel-specific: 0x0A is GIP_CMD_LED but for wheels it's wheel requests
        case GIP_CMD_LED:
            handle_wheel_request(payload, payload_len);
            break;

        default:
            printf("[GIP] Unhandled cmd 0x%02X (%d bytes)\n", cmd, payload_len);
            break;
    }
}

// ─── TinyUSB Device Callbacks ───

void tud_mount_cb(void) {
    state.mount_time_ms = to_ms_since_boot(get_absolute_time());
    state.announce_count = 0;
    state.last_announce_ms = 0;
    state.metadata_sent = false;
    state.device_started = false;
    chunked_tx.active = false;
    printf("[GIP] USB device mounted by host\n");
}

void tud_umount_cb(void) {
    printf("[GIP] USB device unmounted\n");
    state.device_started = false;
    state.metadata_sent = false;
    state.announce_count = 0;
    state.mount_time_ms = 0;
    chunked_tx.active = false;
}

// ─── Public API ───

void gip_init(void) {
    memset(&state, 0, sizeof(state));
    memset(&chunked_tx, 0, sizeof(chunked_tx));
    state.last_keepalive_ms = to_ms_since_boot(get_absolute_time());
    printf("[GIP] Protocol state machine initialized\n");
}

void gip_task(void) {
    // Read incoming data from Xbox via vendor endpoint
    if (tud_vendor_available()) {
        uint32_t count = tud_vendor_read(rx_buf, sizeof(rx_buf));
        if (count > 0) {
            dispatch_gip_packet(rx_buf, (uint16_t)count);
        }
    }

    uint32_t now = to_ms_since_boot(get_absolute_time());

    // Drive chunked transfer state machine
    if (chunked_tx.active && tud_vendor_mounted()) {
        // ACK timeout: give up waiting after 2s
        if (chunked_tx.waiting_ack) {
            if (now - chunked_tx.ack_wait_start >= GIP_ACK_TIMEOUT_MS) {
                printf("[GIP] ACK timeout, continuing\n");
                chunked_tx.waiting_ack = false;
            }
        }

        // Send next chunk with timing (35ms between chunks, not waiting for ACK)
        if (!chunked_tx.waiting_ack &&
            (now - chunked_tx.last_chunk_ms >= GIP_CHUNK_INTERVAL_MS)) {
            chunked_tx_send_next();

            // If chunked transfer just completed, mark metadata as sent
            if (!chunked_tx.active && chunked_tx.command == GIP_CMD_IDENTIFY) {
                state.metadata_sent = true;
            }
        }
    }

    // ANNOUNCE: send every 500ms until Xbox responds with IDENTIFY
    if (!state.metadata_sent && !chunked_tx.active &&
        tud_vendor_mounted() && state.mount_time_ms > 0) {
        if (now - state.last_announce_ms >= ANNOUNCE_INTERVAL_MS) {
            handle_hello_request();
            state.last_announce_ms = now;
        }
    }

    // Keepalive: only after auth complete, every 15s, matches GP2040-CE format
    if (state.authenticated && (now - state.last_keepalive_ms >= GIP_KEEPALIVE_INTERVAL_MS)) {
        uint8_t keepalive_payload[] = {0x80, 0x00, 0x00, 0x00};
        gip_send(GIP_CMD_STATUS, GIP_FLAG_INTERNAL, keepalive_payload,
                 sizeof(keepalive_payload));
        state.last_keepalive_ms = now;
    }
}

void gip_send_input_report(const uint8_t *payload, uint8_t len) {
    gip_send(GIP_CMD_INPUT, 0x00, payload, len);
}

void gip_send_config_report(const uint8_t *payload, uint8_t len) {
    gip_send(GIP_CMD_CONFIG, 0x00, payload, len);
}

void gip_send_hello(void) {
    handle_hello_request();
}

uint8_t gip_next_seq(uint8_t command) {
    uint8_t seq = ++state.seq_counters[command];
    if (seq == 0) seq = ++state.seq_counters[command];
    return seq;
}

// ─── Auth Relay API (called by main loop) ───

uint16_t gip_get_pending_auth(uint8_t *buf, uint16_t buf_max) {
    if (!auth_challenge_pending) return 0;

    uint16_t len = auth_challenge_len;
    if (len > buf_max) len = buf_max;
    memcpy(buf, auth_challenge_buf, len);

    auth_challenge_pending = false;
    auth_challenge_len = 0;
    return len;
}

void gip_submit_auth_response(const uint8_t *response, uint16_t len) {
    if (len == 0) {
        printf("[GIP] Auth response empty — auth failed\n");
        return;
    }

    printf("[GIP] Sending auth response to Xbox (%d bytes)\n", len);
    gip_send(GIP_CMD_AUTHENTICATE, 0x00, response, len);
    state.authenticated = true;
    printf("[GIP] Auth response sent — authenticated!\n");
}

bool gip_is_authenticated(void) {
    return state.authenticated;
}

void gip_get_debug_state(gip_debug_state_t *out) {
    out->flags = (tud_vendor_mounted() ? 0x01 : 0)
               | (state.metadata_sent   ? 0x02 : 0)
               | (state.device_started  ? 0x04 : 0)
               | (state.authenticated   ? 0x08 : 0);
    out->announce_count = (uint8_t)(state.announce_count & 0xFF);
    out->auth_host = auth_host_connected() ? 0x01 : 0;
    out->last_rx_cmd = last_rx_cmd;
}
