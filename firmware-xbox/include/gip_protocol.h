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

// ─── GIP Protocol State ───

typedef struct {
    uint8_t seq_counters[256];  // Per-command-ID sequence numbers
    bool    device_started;     // True after host sends Start
    bool    metadata_sent;      // True after metadata exchange complete
    bool    authenticated;      // True after auth passes
    uint32_t last_keepalive_ms; // Timestamp of last keepalive
} gip_state_t;

// ─── Xbox VID/PID ───

#define XBOX_VID            0x045E  // Microsoft
#define XBOX_PID            0x0B12  // Xbox Series X|S controller (wired)

// ─── Keepalive ───

#define GIP_KEEPALIVE_INTERVAL_MS   1500

// ─── Public API ───

void gip_init(void);
void gip_task(void);    // Process incoming GIP commands
void gip_send_input_report(const uint8_t *payload, uint8_t len);
void gip_send_hello(void);
uint8_t gip_next_seq(uint8_t command);

#endif // GIP_PROTOCOL_H
