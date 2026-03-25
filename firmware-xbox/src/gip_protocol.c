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
#include "spi_protocol.h"
#include "tusb.h"

#include <string.h>
#include <stdio.h>

// ─── State ───

static gip_state_t state;
static uint8_t rx_buf[64];   // Incoming GIP packets from Xbox
static uint8_t tx_buf[64];   // Outgoing GIP packets to Xbox

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

// ─── Packet Building ───

static uint8_t gip_build_header(uint8_t *buf, uint8_t cmd, uint8_t flags,
                                 uint16_t payload_len) {
    buf[0] = cmd;
    buf[1] = flags;  // client=0, flags in upper nibble
    buf[2] = gip_next_seq(cmd);
    uint8_t len_bytes = leb128_encode(payload_len, &buf[3]);
    return 3 + len_bytes;  // Header size
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

// ─── Protocol Handlers ───

static void handle_hello_request(void) {
    // Xbox asks us to identify — send Hello Device (0x02)
    // Payload: device capabilities announcement
    // For now, send a minimal hello that triggers metadata exchange
    uint8_t hello_payload[] = {
        0x20,       // Device Flags (USB, wired)
        0x00, 0x01, // Firmware version major.minor (0.1)
        0x00, 0x01, // Hardware version major.minor (0.1)
    };
    gip_send(GIP_CMD_ANNOUNCE, 0x00, hello_payload, sizeof(hello_payload));
    printf("[GIP] Sent Hello Device\n");
}

static void handle_metadata_request(const uint8_t *payload, uint16_t len) {
    (void)payload; (void)len;

    // Send the compiled metadata blob
    // May need fragmentation if > ~60 bytes per packet
    const uint8_t *blob = gip_get_metadata_blob();
    uint16_t blob_len = gip_get_metadata_len();

    if (blob_len <= 56) {
        // Fits in a single packet
        gip_send(GIP_CMD_IDENTIFY, 0x00, blob, blob_len);
    } else {
        // Fragment: first chunk with CHUNK_START flag
        uint16_t offset = 0;
        uint16_t chunk_size = 56;  // Leave room for header
        bool first = true;

        while (offset < blob_len) {
            uint16_t remaining = blob_len - offset;
            uint16_t this_chunk = (remaining < chunk_size) ? remaining : chunk_size;
            bool last = (offset + this_chunk >= blob_len);

            uint8_t flags = 0;
            if (first) {
                flags = GIP_FLAG_CHUNK_START | GIP_FLAG_CHUNKED;
                first = false;
            } else if (!last) {
                flags = GIP_FLAG_CHUNKED;
            }
            // Last chunk has no chunked flag

            gip_send(GIP_CMD_IDENTIFY, flags, &blob[offset], this_chunk);
            offset += this_chunk;
        }
    }

    state.metadata_sent = true;
    printf("[GIP] Sent metadata (%d bytes, %s)\n",
           blob_len, blob_len > 56 ? "fragmented" : "single packet");
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
        default:
            printf("[GIP] Unknown power state: 0x%02X\n", power_state);
            break;
    }
}

static void handle_auth(const uint8_t *payload, uint16_t len) {
    printf("[GIP] Auth challenge received (%d bytes)\n", len);

    if (len > SPI_AUTH_MAX_PAYLOAD) {
        printf("[GIP] ERROR: auth payload too large (%d > %d)\n",
               len, SPI_AUTH_MAX_PAYLOAD);
        return;
    }

    // Buffer the challenge for the main loop to fragment into SPI frames
    memcpy(auth_challenge_buf, payload, len);
    auth_challenge_len = len;
    auth_challenge_pending = true;

    printf("[GIP] Auth challenge buffered, pending SPI relay\n");
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
    // uint8_t seq = packet[2]; // Tracked internally
    uint8_t len_bytes;
    uint16_t payload_len = leb128_decode(&packet[3], &len_bytes);
    uint8_t header_len = 3 + len_bytes;

    const uint8_t *payload = &packet[header_len];
    (void)flags;

    switch (cmd) {
        case GIP_CMD_ACK:
            // Acknowledgment — no action needed for now
            break;
        case GIP_CMD_IDENTIFY:
            handle_metadata_request(payload, payload_len);
            break;
        case GIP_CMD_POWER:
            handle_power_state(payload, payload_len);
            break;
        case GIP_CMD_AUTHENTICATE:
            handle_auth(payload, payload_len);
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

// ─── Public API ───

void gip_init(void) {
    memset(&state, 0, sizeof(state));
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

    // Send Hello if not yet started
    uint32_t now = to_ms_since_boot(get_absolute_time());
    if (!state.device_started && tud_vendor_mounted()) {
        static uint32_t last_hello = 0;
        if (now - last_hello >= 500) {
            handle_hello_request();
            last_hello = now;
        }
    }

    // Keepalive: send status report periodically
    if (state.device_started && (now - state.last_keepalive_ms >= GIP_KEEPALIVE_INTERVAL_MS)) {
        // Send bus-powered status (0x80)
        uint8_t status_payload[] = {0x00, 0x80};
        gip_send(GIP_CMD_STATUS, 0x00, status_payload, sizeof(status_payload));
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
