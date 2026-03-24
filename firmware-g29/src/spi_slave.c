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
 * SPI1 on RP2040 #2:
 *   GPIO 8  = MISO (slave output to master)
 *   GPIO 9  = CS   (chip select from master)
 *   GPIO 10 = SCK  (clock from master)
 *   GPIO 11 = MOSI (slave input from master)
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

// DMA channels
static int dma_tx_chan = -1;
static int dma_rx_chan = -1;

static bool initialized = false;

void spi_slave_init(void) {
    // Initialize SPI1 in slave mode
    spi_init(SPI_PORT, SPI_CLOCK_HZ);
    spi_set_slave(SPI_PORT, true);
    spi_set_format(SPI_PORT, 8, SPI_CPOL_0, SPI_CPHA_0, SPI_MSB_FIRST);

    // Configure GPIO
    gpio_set_function(SPI_MISO_PIN, GPIO_FUNC_SPI);  // MISO (output)
    gpio_set_function(SPI_CS_PIN,   GPIO_FUNC_SPI);  // CS
    gpio_set_function(SPI_SCK_PIN,  GPIO_FUNC_SPI);  // SCK
    gpio_set_function(SPI_MOSI_PIN, GPIO_FUNC_SPI);  // MOSI (input)

    // Clear buffers
    memset(tx_buf, 0, sizeof(tx_buf));
    memset(&rx_buf, 0, sizeof(rx_buf));
    memset((void *)&last_valid_rx, 0, sizeof(last_valid_rx));

    // Claim DMA channels
    dma_tx_chan = dma_claim_unused_channel(true);
    dma_rx_chan = dma_claim_unused_channel(true);

    // Configure TX DMA (slave → master via MISO)
    dma_channel_config tx_cfg = dma_channel_get_default_config(dma_tx_chan);
    channel_config_set_transfer_data_size(&tx_cfg, DMA_SIZE_8);
    channel_config_set_dreq(&tx_cfg, spi_get_dreq(SPI_PORT, true));  // TX DREQ
    channel_config_set_read_increment(&tx_cfg, true);
    channel_config_set_write_increment(&tx_cfg, false);

    // Configure RX DMA (master → slave via MOSI)
    dma_channel_config rx_cfg = dma_channel_get_default_config(dma_rx_chan);
    channel_config_set_transfer_data_size(&rx_cfg, DMA_SIZE_8);
    channel_config_set_dreq(&rx_cfg, spi_get_dreq(SPI_PORT, false)); // RX DREQ
    channel_config_set_read_increment(&rx_cfg, false);
    channel_config_set_write_increment(&rx_cfg, true);

    // Start first DMA transfer
    dma_channel_configure(dma_tx_chan, &tx_cfg,
                          &spi_get_hw(SPI_PORT)->dr,    // Write to SPI TX FIFO
                          &tx_buf[0],                    // Read from TX buffer 0
                          SPI_FRAME_SIZE,                // Transfer size
                          false);                        // Don't start yet

    dma_channel_configure(dma_rx_chan, &rx_cfg,
                          &rx_buf,                       // Write to RX buffer
                          &spi_get_hw(SPI_PORT)->dr,    // Read from SPI RX FIFO
                          SPI_FRAME_SIZE,                // Transfer size
                          false);                        // Don't start yet

    // Start both channels — they'll wait for the master to clock data
    dma_start_channel_mask((1u << dma_tx_chan) | (1u << dma_rx_chan));

    initialized = true;
    printf("[SPI] Slave initialized on SPI1, DMA channels %d/%d\n",
           dma_tx_chan, dma_rx_chan);
}

void spi_slave_set_tx_frame(const spi_upstream_frame_t *frame) {
    // Write to the inactive buffer
    uint8_t inactive = 1 - tx_active;
    memcpy(&tx_buf[inactive], frame, sizeof(spi_upstream_frame_t));
}

bool spi_slave_task(void) {
    if (!initialized) return false;

    // Check if DMA transfer completed (master clocked a full frame)
    if (dma_channel_is_busy(dma_rx_chan)) {
        return false;  // Still in progress
    }

    // Transfer complete — validate received frame
    rx_count++;
    if (spi_validate_frame((const uint8_t *)&rx_buf)) {
        memcpy((void *)&last_valid_rx, &rx_buf, sizeof(rx_buf));
        rx_valid = true;
    } else {
        rx_errors++;
    }

    // Swap TX buffer
    tx_active = 1 - tx_active;

    // Restart DMA for next frame
    dma_channel_set_read_addr(dma_tx_chan, &tx_buf[tx_active], false);
    dma_channel_set_write_addr(dma_rx_chan, &rx_buf, false);
    dma_channel_set_trans_count(dma_tx_chan, SPI_FRAME_SIZE, false);
    dma_channel_set_trans_count(dma_rx_chan, SPI_FRAME_SIZE, false);
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
