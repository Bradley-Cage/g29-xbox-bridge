/*
 * DEPRECATED: Auth relay moved to Board X (firmware-xbox/src/auth_host.c).
 * This file is not compiled. It remains on disk for reference only.
 */

/**
 * auth_relay.c — PowerA Controller GIP Host + Auth Passthrough
 *
 * Connects to an intact PowerA Xbox controller via PIO-USB host port 1
 * (GPIO 2/3). Acts as a minimal GIP host to bring the controller into
 * an auth-ready state, then transparently relays GIP 0x06 auth packets
 * between the Xbox (via SPI from RP2040 #1) and the PowerA's security chip.
 *
 * This is the same approach used by GP2040-CE, Santroller, and OGX-Mini.
 * Auth payloads are opaque — no crypto needed on our side.
 *
 * GIP communication uses vendor bulk/interrupt endpoints, NOT control
 * transfers. TinyUSB's vendor host class is broken (pipe_handle_t issue),
 * so we use raw endpoint access: tuh_edpt_open() + tuh_edpt_xfer().
 */

#include "auth_relay.h"
#include "bridge_config.h"
#include "spi_protocol.h"
#include "pio_usb.h"
#include "tusb.h"

#include "pico/stdlib.h"
#include "hardware/watchdog.h"
#include <stdio.h>
#include <string.h>

// ─── GIP Host State Machine ───

typedef enum {
    GIP_HOST_IDLE,              // No controller connected
    GIP_HOST_OPENING_ENDPOINTS, // Fetching config descriptor to find endpoints
    GIP_HOST_WAIT_ANNOUNCE,     // Waiting for PowerA's Hello (0x02)
    GIP_HOST_READY,             // GIP startup done, ready for auth relay
} gip_host_state_t;

// ─── Device State ───

static bool auth_connected = false;
static uint8_t auth_dev_addr = 0;
static uint16_t auth_vid = 0;
static uint16_t auth_pid = 0;

// Endpoint info
static uint8_t ep_in_addr = 0;      // Bulk/interrupt IN from PowerA
static uint8_t ep_out_addr = 0;     // Bulk/interrupt OUT to PowerA
static bool endpoints_open = false;

// GIP host state
static gip_host_state_t gip_state = GIP_HOST_IDLE;
static uint8_t gip_seq_counters[256];  // Per-command sequence tracking

// RX buffer for GIP packets from PowerA
static uint8_t gip_rx_buf[64];
static volatile bool rx_pending = false;
static volatile uint32_t rx_actual_len = 0;

// TX state
static uint8_t gip_tx_buf[64];
static volatile bool tx_pending = false;

// Auth challenge/response buffers
static uint8_t auth_challenge_buf[SPI_AUTH_MAX_PAYLOAD];
static uint16_t auth_challenge_len = 0;
static uint16_t auth_challenge_offset = 0;  // For chunked TX
static bool auth_challenge_queued = false;

static uint8_t auth_response_buf[SPI_AUTH_MAX_PAYLOAD];
static uint16_t auth_response_len = 0;
static uint16_t auth_response_offset = 0;   // For chunked RX reassembly
static bool auth_response_ready = false;

// Config descriptor buffer (for endpoint discovery)
static uint8_t config_desc_buf[256];

// ─── GIP Packet Helpers ───

static uint8_t gip_host_next_seq(uint8_t cmd) {
    uint8_t seq = ++gip_seq_counters[cmd];
    if (seq == 0) seq = ++gip_seq_counters[cmd];  // Skip 0
    return seq;
}

// Build a GIP header and return total header length
static uint8_t gip_build(uint8_t *buf, uint8_t cmd, uint8_t flags,
                          const uint8_t *payload, uint16_t payload_len) {
    buf[0] = cmd;
    buf[1] = flags;
    buf[2] = gip_host_next_seq(cmd);

    // LEB128 length
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

// Parse GIP header, returns payload pointer and length
static bool gip_parse(const uint8_t *pkt, uint16_t pkt_len,
                       uint8_t *cmd, uint8_t *flags, uint8_t *seq,
                       const uint8_t **payload, uint16_t *payload_len) {
    if (pkt_len < 4) return false;

    *cmd = pkt[0];
    *flags = pkt[1];
    *seq = pkt[2];

    // LEB128 decode
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

// ─── USB Transfer Callbacks ───

static void on_rx_complete(tuh_xfer_t *xfer) {
    if (xfer->result == XFER_RESULT_SUCCESS) {
        rx_actual_len = xfer->actual_len;
        rx_pending = true;
    } else {
        rx_actual_len = 0;
        rx_pending = false;
    }
}

static void on_tx_complete(tuh_xfer_t *xfer) {
    (void)xfer;
    tx_pending = false;
}

// Start an async read from the PowerA's IN endpoint
static bool start_rx(void) {
    if (!endpoints_open || ep_in_addr == 0 || rx_pending) return false;

    tuh_xfer_t xfer = {
        .daddr       = auth_dev_addr,
        .ep_addr     = ep_in_addr,
        .buffer      = gip_rx_buf,
        .buflen      = sizeof(gip_rx_buf),
        .complete_cb = on_rx_complete,
        .user_data   = 0,
    };
    return tuh_edpt_xfer(&xfer);
}

// Send a GIP packet to the PowerA's OUT endpoint (async)
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

// ─── Config Descriptor Parsing — Find GIP Endpoints ───

static void on_config_desc_received(tuh_xfer_t *xfer) {
    if (xfer->result != XFER_RESULT_SUCCESS || xfer->actual_len < 9) {
        printf("[AUTH] Failed to get config descriptor\n");
        gip_state = GIP_HOST_IDLE;
        return;
    }

    uint16_t total_len = xfer->actual_len;
    uint8_t *p = config_desc_buf;
    uint8_t *end = p + total_len;

    // Walk descriptors looking for GIP interface (class 0xFF/0x47/0xD0)
    bool found_iface = false;

    while (p < end) {
        uint8_t desc_len = p[0];
        uint8_t desc_type = p[1];

        if (desc_len == 0) break;  // Malformed

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

            if (ep_addr & 0x80) {
                // IN endpoint
                ep_in_addr = ep_addr;
                tuh_edpt_open(auth_dev_addr, ep);
                printf("[AUTH] Opened IN endpoint 0x%02X\n", ep_addr);
            } else {
                // OUT endpoint
                ep_out_addr = ep_addr;
                tuh_edpt_open(auth_dev_addr, ep);
                printf("[AUTH] Opened OUT endpoint 0x%02X\n", ep_addr);
            }
        }

        p += desc_len;
    }

    if (ep_in_addr && ep_out_addr) {
        endpoints_open = true;
        gip_state = GIP_HOST_WAIT_ANNOUNCE;
        printf("[AUTH] Endpoints ready — waiting for PowerA Hello\n");

        // Start listening for GIP packets
        start_rx();
    } else {
        printf("[AUTH] ERROR: GIP endpoints not found in config descriptor\n");
        printf("[AUTH]   ep_in=0x%02X ep_out=0x%02X found_iface=%d\n",
               ep_in_addr, ep_out_addr, found_iface);
        gip_state = GIP_HOST_IDLE;
    }
}

// ─── GIP Host Protocol Handlers ───

static void handle_powerA_announce(const uint8_t *payload, uint16_t len) {
    (void)payload; (void)len;
    printf("[AUTH] PowerA sent Hello/Announce — sending Power On\n");

    // Send Power On (command 0x05, payload = state ON)
    uint8_t power_payload[] = { GIP_HOST_STATE_ON };
    send_gip(GIP_HOST_CMD_POWER, 0x00, power_payload, sizeof(power_payload));

    gip_state = GIP_HOST_READY;
    printf("[AUTH] GIP host ready — auth relay active\n");
}

static void handle_powerA_auth_response(const uint8_t *payload, uint16_t len,
                                         uint8_t flags) {
    bool chunked = (flags & GIP_HOST_FLAG_CHUNKED) != 0;
    bool chunk_start = (flags & GIP_HOST_FLAG_CHUNK_START) != 0;

    if (!chunked) {
        // Single-packet response — copy directly
        if (len <= sizeof(auth_response_buf)) {
            memcpy(auth_response_buf, payload, len);
            auth_response_len = len;
            auth_response_ready = true;
            printf("[AUTH] Auth response received (%d bytes, single)\n", len);
        }
        return;
    }

    // Chunked response
    if (chunk_start) {
        // First chunk — reset reassembly
        auth_response_offset = 0;
        auth_response_len = 0;
        auth_response_ready = false;
    }

    if (auth_response_offset + len <= sizeof(auth_response_buf)) {
        memcpy(&auth_response_buf[auth_response_offset], payload, len);
        auth_response_offset += len;
    }

    // If this is the last chunk (chunked flag set but no chunk_start,
    // and next packet won't have chunked flag — detected by checking
    // if we got a non-chunk packet next, or by tracking total).
    // For safety, we mark ready when we receive a non-chunked packet
    // or when payload is smaller than max packet payload size.
    uint16_t max_payload_in_packet = 64 - 4;  // 4-byte GIP header
    if (len < max_payload_in_packet) {
        // Last chunk (not a full packet)
        auth_response_len = auth_response_offset;
        auth_response_ready = true;
        printf("[AUTH] Auth response reassembled (%d bytes, chunked)\n",
               auth_response_len);
    }
}

static void process_gip_packet(const uint8_t *pkt, uint16_t pkt_len) {
    uint8_t cmd, flags, seq;
    const uint8_t *payload;
    uint16_t payload_len;

    if (!gip_parse(pkt, pkt_len, &cmd, &flags, &seq, &payload, &payload_len)) {
        return;
    }

    switch (cmd) {
        case GIP_HOST_CMD_ANNOUNCE:
            handle_powerA_announce(payload, payload_len);
            break;

        case GIP_HOST_CMD_AUTHENTICATE:
            handle_powerA_auth_response(payload, payload_len, flags);
            break;

        case GIP_HOST_CMD_STATUS:
            // PowerA sending status — acknowledge but ignore content
            break;

        case GIP_HOST_CMD_IDENTIFY:
            // Metadata from PowerA — we don't need it, just ACK
            printf("[AUTH] PowerA sent metadata (%d bytes)\n", payload_len);
            break;

        default:
            // Input reports (0x20), virtual key (0x07), etc — ignore
            break;
    }
}

// ─── Auth Challenge TX (chunked GIP 0x06 to PowerA) ───

static void send_next_auth_chunk(void) {
    if (!auth_challenge_queued || tx_pending) return;

    uint16_t remaining = auth_challenge_len - auth_challenge_offset;
    if (remaining == 0) {
        auth_challenge_queued = false;
        return;
    }

    // Max payload per GIP packet: 64 - 4 (header) = 60 bytes
    uint16_t max_chunk = 60;
    uint16_t chunk_len = remaining < max_chunk ? remaining : max_chunk;
    bool first = (auth_challenge_offset == 0);
    bool fits_single = (auth_challenge_len <= max_chunk);

    uint8_t flags = 0;
    if (!fits_single) {
        if (first) {
            flags = GIP_HOST_FLAG_CHUNK_START | GIP_HOST_FLAG_CHUNKED;
        } else if (remaining > max_chunk) {
            flags = GIP_HOST_FLAG_CHUNKED;
        }
        // Last chunk: no chunked flags
    }

    if (send_gip(GIP_HOST_CMD_AUTHENTICATE, flags,
                  &auth_challenge_buf[auth_challenge_offset], chunk_len)) {
        auth_challenge_offset += chunk_len;
        if (auth_challenge_offset >= auth_challenge_len) {
            printf("[AUTH] Auth challenge fully sent to PowerA (%d bytes)\n",
                   auth_challenge_len);
            auth_challenge_queued = false;
        }
    }
}

// ─── Device Notification Handlers ───

void auth_relay_on_mount(uint8_t dev_addr, uint16_t vid, uint16_t pid) {
    auth_dev_addr = dev_addr;
    auth_vid = vid;
    auth_pid = pid;
    auth_connected = true;

    printf("[AUTH] Controller mounted: addr=%d VID=%04X PID=%04X\n",
           dev_addr, vid, pid);

    // Fetch config descriptor to find GIP endpoints
    gip_state = GIP_HOST_OPENING_ENDPOINTS;
    memset(gip_seq_counters, 0, sizeof(gip_seq_counters));

    tuh_descriptor_get_configuration(dev_addr, 0, config_desc_buf,
                                      sizeof(config_desc_buf),
                                      on_config_desc_received, 0);
}

void auth_relay_on_umount(uint8_t dev_addr) {
    if (dev_addr == auth_dev_addr) {
        printf("[AUTH] Controller disconnected (addr=%d)\n", dev_addr);
        auth_connected = false;
        auth_dev_addr = 0;
        auth_vid = 0;
        auth_pid = 0;
        ep_in_addr = 0;
        ep_out_addr = 0;
        endpoints_open = false;
        gip_state = GIP_HOST_IDLE;

        // Clear any pending auth state
        auth_challenge_queued = false;
        auth_response_ready = false;
        auth_response_len = 0;
        rx_pending = false;
        tx_pending = false;
    }
}

// ─── Public API ───

bool auth_relay_init(void) {
    int result = pio_usb_host_add_port(AUTH_USB_DP_PIN, PIO_USB_PINOUT_DPDM);
    if (result < 0) {
        printf("[AUTH] Failed to add root port 1 on GPIO %d/%d\n",
               AUTH_USB_DP_PIN, AUTH_USB_DM_PIN);
        return false;
    }

    printf("[AUTH] Root port 1 added on GPIO %d/%d — plug in PowerA controller\n",
           AUTH_USB_DP_PIN, AUTH_USB_DM_PIN);
    return true;
}

void auth_relay_task(void) {
    if (!auth_connected || !endpoints_open) return;

    // Process received GIP packet
    if (rx_pending) {
        rx_pending = false;
        if (rx_actual_len > 0) {
            process_gip_packet(gip_rx_buf, rx_actual_len);
        }
        // Start next read
        start_rx();
    }

    // Send queued auth challenge chunks
    if (auth_challenge_queued && !tx_pending) {
        send_next_auth_chunk();
    }
}

bool auth_relay_connected(void) {
    return auth_connected && gip_state == GIP_HOST_READY;
}

bool auth_relay_send_challenge(const uint8_t *challenge, uint16_t len) {
    if (!auth_connected || gip_state != GIP_HOST_READY) {
        printf("[AUTH] Cannot send challenge — not ready (state=%d)\n", gip_state);
        return false;
    }

    if (len > sizeof(auth_challenge_buf)) {
        printf("[AUTH] Challenge too large (%d > %d)\n", len,
               (int)sizeof(auth_challenge_buf));
        return false;
    }

    memcpy(auth_challenge_buf, challenge, len);
    auth_challenge_len = len;
    auth_challenge_offset = 0;
    auth_challenge_queued = true;

    // Clear any stale response
    auth_response_ready = false;
    auth_response_len = 0;
    auth_response_offset = 0;

    printf("[AUTH] Queued %d-byte challenge for PowerA\n", len);
    return true;
}

uint16_t auth_relay_get_response(uint8_t *response, uint16_t response_max) {
    if (!auth_response_ready || auth_response_len == 0) return 0;

    uint16_t copy_len = auth_response_len;
    if (copy_len > response_max) copy_len = response_max;
    memcpy(response, auth_response_buf, copy_len);

    // Clear after retrieval
    auth_response_ready = false;
    auth_response_len = 0;
    auth_response_offset = 0;

    return copy_len;
}

uint16_t auth_relay_get_vid(void) {
    return auth_vid;
}

uint16_t auth_relay_get_pid(void) {
    return auth_pid;
}
