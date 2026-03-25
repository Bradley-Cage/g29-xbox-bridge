/**
 * spi_master.c — DMA-based SPI Master Communication to RP2040 #2
 *
 * Non-blocking SPI transfers using DMA. Core 0 kicks off the transfer,
 * then runs TinyUSB/GIP tasks while DMA handles the bus. Checks for
 * completion before reading the received frame.
 *
 * SPI0 on RP2040 #1 (DWEII board — GPIO 16-25 have NO pads):
 *   GPIO 2 = SCK                     — SPI0_SCK
 *   GPIO 3 = MOSI (data to slave)    — SPI0_TX
 *   GPIO 4 = MISO (data from slave)  — SPI0_RX
 *   GPIO 5 = CS   (active low)       — manual GPIO
 */

#include "spi_protocol.h"
#include "hardware/spi.h"
#include "hardware/dma.h"
#include "hardware/gpio.h"
#include "pico/stdlib.h"

#include <string.h>
#include <stdio.h>

#define SPI_MASTER_PORT     spi0
#define SPI_MASTER_SCK      2
#define SPI_MASTER_MOSI     3
#define SPI_MASTER_MISO     4
#define SPI_MASTER_CS       5

static int dma_tx_chan = -1;
static int dma_rx_chan = -1;
static bool transfer_active = false;
static bool initialized = false;

// Buffers — the DMA reads/writes these, so they must persist during transfer
static spi_downstream_frame_t tx_frame_dma;
static spi_upstream_frame_t rx_frame_dma;

// Latest valid received frame
static spi_upstream_frame_t last_valid_rx;
static bool rx_valid = false;
static uint32_t rx_count = 0;
static uint32_t rx_errors = 0;

void spi_master_init(void) {
    // Init SPI0 at 10MHz
    spi_init(SPI_MASTER_PORT, SPI_CLOCK_HZ);
    spi_set_format(SPI_MASTER_PORT, 8, SPI_CPOL_0, SPI_CPHA_0, SPI_MSB_FIRST);

    // GPIO
    gpio_set_function(SPI_MASTER_SCK,  GPIO_FUNC_SPI);
    gpio_set_function(SPI_MASTER_MOSI, GPIO_FUNC_SPI);
    gpio_set_function(SPI_MASTER_MISO, GPIO_FUNC_SPI);

    // Manual CS (not hardware — we need to control it around DMA)
    gpio_init(SPI_MASTER_CS);
    gpio_set_dir(SPI_MASTER_CS, GPIO_OUT);
    gpio_put(SPI_MASTER_CS, 1);  // Inactive

    // Claim DMA channels
    dma_tx_chan = dma_claim_unused_channel(true);
    dma_rx_chan = dma_claim_unused_channel(true);

    // TX DMA config
    dma_channel_config tx_cfg = dma_channel_get_default_config(dma_tx_chan);
    channel_config_set_transfer_data_size(&tx_cfg, DMA_SIZE_8);
    channel_config_set_dreq(&tx_cfg, spi_get_dreq(SPI_MASTER_PORT, true));
    channel_config_set_read_increment(&tx_cfg, true);
    channel_config_set_write_increment(&tx_cfg, false);
    dma_channel_configure(dma_tx_chan, &tx_cfg,
                          &spi_get_hw(SPI_MASTER_PORT)->dr,
                          NULL, SPI_FRAME_SIZE, false);

    // RX DMA config
    dma_channel_config rx_cfg = dma_channel_get_default_config(dma_rx_chan);
    channel_config_set_transfer_data_size(&rx_cfg, DMA_SIZE_8);
    channel_config_set_dreq(&rx_cfg, spi_get_dreq(SPI_MASTER_PORT, false));
    channel_config_set_read_increment(&rx_cfg, false);
    channel_config_set_write_increment(&rx_cfg, true);
    dma_channel_configure(dma_rx_chan, &rx_cfg,
                          NULL, &spi_get_hw(SPI_MASTER_PORT)->dr,
                          SPI_FRAME_SIZE, false);

    memset(&last_valid_rx, 0, sizeof(last_valid_rx));
    initialized = true;
    printf("[SPI] Master initialized, DMA channels %d/%d\n", dma_tx_chan, dma_rx_chan);
}

void spi_master_start_transfer(const spi_downstream_frame_t *frame) {
    if (!initialized || transfer_active) return;

    // Copy frame to DMA buffer
    memcpy(&tx_frame_dma, frame, sizeof(spi_downstream_frame_t));

    // Assert CS
    gpio_put(SPI_MASTER_CS, 0);

    // Configure and start both DMA channels
    dma_channel_set_read_addr(dma_tx_chan, &tx_frame_dma, false);
    dma_channel_set_write_addr(dma_rx_chan, &rx_frame_dma, false);
    dma_channel_set_trans_count(dma_tx_chan, SPI_FRAME_SIZE, false);
    dma_channel_set_trans_count(dma_rx_chan, SPI_FRAME_SIZE, false);

    // Start both simultaneously
    dma_start_channel_mask((1u << dma_tx_chan) | (1u << dma_rx_chan));
    transfer_active = true;
}

bool spi_master_transfer_done(void) {
    if (!transfer_active) return true;
    return !dma_channel_is_busy(dma_rx_chan);
}

bool spi_master_finish_transfer(void) {
    if (!transfer_active) return false;
    if (dma_channel_is_busy(dma_rx_chan)) return false;

    // Deassert CS
    gpio_put(SPI_MASTER_CS, 1);
    transfer_active = false;

    // Validate received frame
    rx_count++;
    if (spi_validate_frame((const uint8_t *)&rx_frame_dma)) {
        memcpy(&last_valid_rx, &rx_frame_dma, sizeof(rx_frame_dma));
        rx_valid = true;
        return true;
    } else {
        rx_errors++;
        return false;
    }
}

bool spi_master_get_rx_frame(spi_upstream_frame_t *out) {
    if (!rx_valid) return false;
    memcpy(out, &last_valid_rx, sizeof(spi_upstream_frame_t));
    return true;
}

uint32_t spi_master_get_rx_count(void) { return rx_count; }
uint32_t spi_master_get_error_count(void) { return rx_errors; }
