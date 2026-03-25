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

// RX buffer for GIP packets from PowerA
static uint8_t gip_rx_buf[64];
static volatile bool rx_pending = false;
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

            if (ep_addr & 0x80) {
                ep_in_addr = ep_addr;
                tuh_edpt_open(auth_dev_addr, ep);
                printf("[AUTH] Opened IN endpoint 0x%02X\n", ep_addr);
            } else {
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
        start_rx();
    } else {
        printf("[AUTH] ERROR: GIP endpoints not found\n");
        gip_state = GIP_HOST_IDLE;
    }
}

// ─── GIP Protocol Handlers ───

static void handle_powerA_announce(const uint8_t *payload, uint16_t len) {
    (void)payload; (void)len;
    printf("[AUTH] PowerA sent Hello — sending Power On\n");

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
        if (len <= sizeof(auth_rx_buf)) {
            memcpy(auth_rx_buf, payload, len);
            auth_rx_len = len;
            auth_rx_ready = true;
            printf("[AUTH] Auth response received (%d bytes, single)\n", len);
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

    switch (cmd) {
        case GIP_HOST_CMD_ANNOUNCE:
            handle_powerA_announce(payload, payload_len);
            break;
        case GIP_HOST_CMD_AUTHENTICATE:
            handle_powerA_auth_response(payload, payload_len, flags);
            break;
        case GIP_HOST_CMD_STATUS:
        case GIP_HOST_CMD_IDENTIFY:
        case GIP_HOST_CMD_INPUT:
            break;  // Ignore
        default:
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

    uint8_t flags = 0;
    if (!fits_single) {
        if (first) {
            flags = GIP_HOST_FLAG_CHUNK_START | GIP_HOST_FLAG_CHUNKED;
        } else if (remaining > max_chunk) {
            flags = GIP_HOST_FLAG_CHUNKED;
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
        tx_pending = false;
    }
}

// ─── Public API ───

bool auth_host_init(void) {
    // Configure PIO-USB for TinyUSB host on port 1 (PIO0)
    pio_usb_configuration_t pio_cfg = PIO_USB_DEFAULT_CONFIG;
    pio_cfg.pin_dp = AUTH_PIO_USB_DP_PIN;
    pio_cfg.pinout = PIO_USB_PINOUT_DPDM;

    tuh_configure(1, TUH_CFGID_RPI_PIO_USB_CONFIGURATION, &pio_cfg);
    tuh_init(1);

    printf("[AUTH] PIO-USB host initialized on GPIO %d/%d\n",
           AUTH_PIO_USB_DP_PIN, AUTH_PIO_USB_DM_PIN);
    return true;
}

void auth_host_task(void) {
    tuh_task();

    if (!auth_connected || !endpoints_open) return;

    // Process received GIP packet
    if (rx_pending) {
        rx_pending = false;
        if (rx_actual_len > 0) {
            process_gip_packet(gip_rx_buf, rx_actual_len);
        }
        start_rx();
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
