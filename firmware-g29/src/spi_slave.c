/**
 * spi_slave.c — SPI Slave Communication to RP2040 #1
 *
 * RP2040 #2 acts as SPI slave. The master (RP2040 #1) clocks
 * 32-byte frames at 1kHz. Full-duplex: we send G29 state upstream
 * while receiving force/command data downstream, simultaneously.
 *
 * DMA is used for the transfer to avoid blocking Core 0.
 * Double-buffered: Core 0 fills the next TX buffer while DMA
 * sends the current one.
 *
 * SPI1 on RP2040 #2 (pin mapping from bridge_config.h):
 *   GPIO 12 = SPI1_RX (MOSI — data from master)
 *   GPIO 9  = SPI1_CSn (chip select from master)
 *   GPIO 10 = SPI1_SCK (clock from master)
 *   GPIO 11 = SPI1_TX (MISO — data to master)
 */

#include "spi_protocol.h"
#include "bridge_config.h"

#include "hardware/spi.h"
#include "hardware/dma.h"
#include "hardware/gpio.h"
#include "pico/stdlib.h"

#include <string.h>
#include <stdio.h>

// Double-buffered TX (upstream: G29 state)
static spi_upstream_frame_t tx_buf[2];
static volatile uint8_t tx_active = 0;  // Which buffer DMA is sending

// Single RX buffer (downstream: force/commands from master)
static spi_downstream_frame_t rx_buf;

// Latest valid received frame (for main loop to read)
static volatile spi_downstream_frame_t last_valid_rx;
static volatile bool rx_valid = false;
static volatile uint32_t rx_count = 0;
static volatile uint32_t rx_errors = 0;

// DMA channels and saved configs (re-applied on every restart)
static int dma_tx_chan = -1;
static int dma_rx_chan = -1;
static dma_channel_config saved_tx_cfg;
static dma_channel_config saved_rx_cfg;

static bool initialized = false;

void spi_slave_init(void) {
    // Initialize SPI1 in slave mode
    spi_init(SPI_PORT, SPI_CLOCK_HZ);
    spi_set_slave(SPI_PORT, true);
    // Mode 3 (CPOL=1, CPHA=1) required — PL022 slave with CPHA=0 freezes
    // the shift register after the first byte when CS stays low.
    spi_set_format(SPI_PORT, 8, SPI_CPOL_1, SPI_CPHA_1, SPI_MSB_FIRST);

    // Configure GPIO
    gpio_set_function(SPI_MISO_PIN, GPIO_FUNC_SPI);  // SPI1_TX (slave output)
    gpio_set_function(SPI_CS_PIN,   GPIO_FUNC_SPI);  // SPI1_CSn
    gpio_set_function(SPI_SCK_PIN,  GPIO_FUNC_SPI);  // SPI1_SCK
    gpio_set_function(SPI_MOSI_PIN, GPIO_FUNC_SPI);  // SPI1_RX (slave input)

    // Flush RX FIFO — master may have clocked bytes during boot
    while (spi_is_readable(SPI_PORT)) {
        (void)spi_get_hw(SPI_PORT)->dr;
    }

    // Clear buffers
    memset(tx_buf, 0, sizeof(tx_buf));
    memset(&rx_buf, 0, sizeof(rx_buf));
    memset((void *)&last_valid_rx, 0, sizeof(last_valid_rx));

    // Claim DMA channels
    dma_tx_chan = dma_claim_unused_channel(true);
    dma_rx_chan = dma_claim_unused_channel(true);

    // Configure TX DMA (slave → master via SPI1_TX/GPIO 11)
    saved_tx_cfg = dma_channel_get_default_config(dma_tx_chan);
    channel_config_set_transfer_data_size(&saved_tx_cfg, DMA_SIZE_8);
    channel_config_set_dreq(&saved_tx_cfg, spi_get_dreq(SPI_PORT, true));
    channel_config_set_read_increment(&saved_tx_cfg, true);
    channel_config_set_write_increment(&saved_tx_cfg, false);

    // Configure RX DMA (master → slave via SPI1_RX/GPIO 12)
    saved_rx_cfg = dma_channel_get_default_config(dma_rx_chan);
    channel_config_set_transfer_data_size(&saved_rx_cfg, DMA_SIZE_8);
    channel_config_set_dreq(&saved_rx_cfg, spi_get_dreq(SPI_PORT, false));
    channel_config_set_read_increment(&saved_rx_cfg, false);
    channel_config_set_write_increment(&saved_rx_cfg, true);

    // Start first DMA transfer
    dma_channel_configure(dma_tx_chan, &saved_tx_cfg,
                          &spi_get_hw(SPI_PORT)->dr,
                          &tx_buf[0],
                          SPI_FRAME_SIZE, false);

    dma_channel_configure(dma_rx_chan, &saved_rx_cfg,
                          &rx_buf,
                          &spi_get_hw(SPI_PORT)->dr,
                          SPI_FRAME_SIZE, false);

    dma_start_channel_mask((1u << dma_tx_chan) | (1u << dma_rx_chan));

    initialized = true;
    printf("[SPI] Slave on SPI1: MOSI=GPIO%d MISO=GPIO%d SCK=GPIO%d CS=GPIO%d DMA=%d/%d\n",
           SPI_MOSI_PIN, SPI_MISO_PIN, SPI_SCK_PIN, SPI_CS_PIN,
           dma_tx_chan, dma_rx_chan);
}

void spi_slave_set_tx_frame(const spi_upstream_frame_t *frame) {
    uint8_t inactive = 1 - tx_active;
    memcpy(&tx_buf[inactive], frame, sizeof(spi_upstream_frame_t));
}

bool spi_slave_task(void) {
    if (!initialized) return false;

    if (dma_channel_is_busy(dma_rx_chan)) {
        return false;
    }

    // Transfer complete — validate received frame
    rx_count++;
    if (spi_validate_frame((const uint8_t *)&rx_buf)) {
        memcpy((void *)&last_valid_rx, &rx_buf, sizeof(rx_buf));
        rx_valid = true;
        // Log first few valid frames
        static uint32_t ok_log = 0;
        if (ok_log < 3) {
            ok_log++;
            uint8_t *p = (uint8_t *)&rx_buf;
            printf("[SPI-RX] OK #%lu: %02X %02X %02X %02X %02X %02X %02X %02X\n",
                   (unsigned long)ok_log,
                   p[0], p[1], p[2], p[3], p[4], p[5], p[6], p[7]);
        }
    } else {
        rx_errors++;
        if (rx_errors <= 5) {
            uint8_t *p = (uint8_t *)&rx_buf;
            uint8_t expected_crc = spi_crc8(p, SPI_FRAME_SIZE - 1);
            printf("[SPI-RX] CRC fail #%lu: got=0x%02X exp=0x%02X | "
                   "%02X %02X %02X %02X %02X %02X %02X %02X\n",
                   (unsigned long)rx_errors, p[SPI_CRC_OFFSET], expected_crc,
                   p[0], p[1], p[2], p[3], p[4], p[5], p[6], p[7]);
        }
    }

    // Swap TX buffer
    tx_active = 1 - tx_active;

    // Flush stale bytes between DMA completion and restart
    while (spi_is_readable(SPI_PORT)) {
        (void)spi_get_hw(SPI_PORT)->dr;
    }

    // Restart DMA with full configure (re-applies saved config each time)
    dma_channel_configure(dma_tx_chan, &saved_tx_cfg,
                          &spi_get_hw(SPI_PORT)->dr,
                          &tx_buf[tx_active],
                          SPI_FRAME_SIZE, false);

    dma_channel_configure(dma_rx_chan, &saved_rx_cfg,
                          &rx_buf,
                          &spi_get_hw(SPI_PORT)->dr,
                          SPI_FRAME_SIZE, false);

    dma_start_channel_mask((1u << dma_tx_chan) | (1u << dma_rx_chan));

    return rx_valid;
}

bool spi_slave_get_rx_frame(spi_downstream_frame_t *out) {
    if (!rx_valid) return false;
    memcpy(out, (void *)&last_valid_rx, sizeof(spi_downstream_frame_t));
    return true;
}

uint32_t spi_slave_get_rx_count(void) { return rx_count; }
uint32_t spi_slave_get_error_count(void) { return rx_errors; }
