/**
 * gip_protocol.h — Xbox GIP (Gaming Input Protocol) Definitions
 */

#ifndef GIP_PROTOCOL_H
#define GIP_PROTOCOL_H

#include <stdint.h>
#include <stdbool.h>

// ─── GIP Command IDs ───

#define GIP_CMD_ACK             0x01
#define GIP_CMD_ANNOUNCE        0x02    // Device → Host: Hello
#define GIP_CMD_STATUS          0x03    // Device → Host: Battery/power status
#define GIP_CMD_IDENTIFY        0x04    // Both: Metadata request/response
#define GIP_CMD_POWER           0x05    // Host → Device: Set device state
#define GIP_CMD_AUTHENTICATE    0x06    // Both: Auth handshake
#define GIP_CMD_VIRTUAL_KEY     0x07    // Device → Host: Guide button etc
#define GIP_CMD_AUDIO_CTRL      0x08
#define GIP_CMD_LED             0x0A    // Host → Device: LED control
#define GIP_CMD_APP_MEMORY      0x0B    // Host → Device: Set application memory
#define GIP_CMD_EQ_STATES       0x0C    // Host → Device: Set equation states
#define GIP_CMD_EQ_SET          0x0D    // Host → Device: Set equation bytecode
#define GIP_CMD_SERIAL          0x1E
#define GIP_CMD_INPUT           0x20    // Device → Host: Wheel state report
#define GIP_CMD_CONFIG          0x21    // Device → Host: Static config report
#define GIP_CMD_AUDIO           0x60

// ─── GIP Wheel Requests (Command 0x0A subtypes) ───

#define GIP_WHEEL_REQ_INITIAL   0x00    // Request config + state reports
#define GIP_WHEEL_REQ_ANGLE     0x01    // Set rotation angle
#define GIP_WHEEL_REQ_POWER     0x02    // Set power level
#define GIP_WHEEL_REQ_VIBRATION 0x03    // Set vibration motors

// ─── GIP Header Flags ───

#define GIP_FLAG_ACK            0x10
#define GIP_FLAG_INTERNAL       0x20
#define GIP_FLAG_CHUNK_START    0x40
#define GIP_FLAG_CHUNKED        0x80

// ─── GIP Device States (Command 0x05) ───

#define GIP_STATE_OFF           0x04
#define GIP_STATE_QUIESCE       0x05
#define GIP_STATE_ON            0x00

// ─── GIP Packet Header ───

typedef struct __attribute__((packed)) {
    uint8_t command;        // GIP_CMD_*
    uint8_t flags;          // Client ID (bits 0-3) + flags (bits 4-7)
    uint8_t sequence;       // Per-command sequence counter (1-255, wraps to 1)
    uint8_t length;         // Payload length (simple) or LEB128 first byte
} gip_header_t;

// ─── XGIP Chunked Transfer ───

#define GIP_MAX_CHUNK_DATA   0x3A  // 58 bytes max payload per chunk
#define GIP_CHUNK_INTERVAL_MS  35  // Minimum ms between chunk sends
#define GIP_ACK_TIMEOUT_MS   2000  // Max wait for ACK before retrying

typedef struct {
    bool     active;            // Chunked transfer in progress
    uint8_t  command;           // GIP command for this transfer
    uint8_t  sequence;          // Base sequence from the request
    const uint8_t *data;        // Pointer to full data blob
    uint16_t data_len;          // Total data length
    uint16_t total_sent;        // Bytes sent so far
    uint16_t total_chunk_len;   // XGIP "hashed" total length
    uint16_t total_chunk_sent;  // XGIP "hashed" running counter
    uint8_t  chunks_sent;       // Number of chunks sent
    uint32_t last_chunk_ms;     // Timestamp of last chunk send
    bool     waiting_ack;       // Blocked waiting for ACK
    uint32_t ack_wait_start;    // When we started waiting for ACK
} gip_chunked_tx_t;

// ─── GIP Protocol State ───

typedef struct {
    uint8_t seq_counters[256];  // Per-command-ID sequence numbers
    bool    device_started;     // True after host sends Start
    bool    metadata_sent;      // True after metadata exchange complete
    bool    authenticated;      // True after auth passes
    uint32_t last_keepalive_ms; // Timestamp of last keepalive
    uint32_t mount_time_ms;     // When USB device was mounted
    uint32_t last_announce_ms;  // Timestamp of last ANNOUNCE sent
    uint16_t announce_count;    // Number of ANNOUNCEs sent (debug)
} gip_state_t;

// ─── Xbox VID/PID ───

// VID/PID: Using PDP (GP2040-CE proven) instead of Microsoft VID.
// Microsoft VID 0x045E requires matching auth certificate on the wire.
#define XBOX_VID            0x0E6F  // PDP (Performance Designed Products)
#define XBOX_PID            0x02A4  // GP2040-CE compatible

// ─── Keepalive ───

#define GIP_KEEPALIVE_INTERVAL_MS   15000  // 15s, matches GP2040-CE

// ─── Public API ───

void gip_init(void);
void gip_task(void);    // Process incoming GIP commands
void gip_send_input_report(const uint8_t *payload, uint8_t len);
void gip_send_config_report(const uint8_t *payload, uint8_t len);
void gip_send_hello(void);
uint8_t gip_next_seq(uint8_t command);

// Auth relay: called by main loop to shuttle auth data over SPI
// Returns pending challenge length (0 if none). Clears pending flag.
uint16_t gip_get_pending_auth(uint8_t *buf, uint16_t buf_max);

// Submit auth response received from RP2040 #2 back to Xbox
void gip_submit_auth_response(const uint8_t *response, uint16_t len);

// Check if authenticated
bool gip_is_authenticated(void);

// Debug: packed GIP state for SPI telemetry (4 bytes)
typedef struct {
    uint8_t flags;          // bit0=mounted, bit1=metadata_sent, bit2=device_started, bit3=authenticated
    uint8_t announce_count; // Number of ANNOUNCEs sent (wraps at 255)
    uint8_t auth_host;      // bit0=PowerA connected
    uint8_t last_rx_cmd;    // Last GIP command received from Xbox
} gip_debug_state_t;

void gip_get_debug_state(gip_debug_state_t *out);

#endif // GIP_PROTOCOL_H
