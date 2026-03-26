// P4 SPI Transport — picosdk-native real-time audio sender
// Link1 (spi1rp ↔ SPI2p4): high-speed audio over spi1 (GPIO 28-31) to the P4.
// Independent of spi0 (SD card).
//
// Protocol: 0xCAFE fingerprint (2 bytes) + p4_spi_request_header (16 bytes)
//           + p4_spi_request2 (276 bytes) + padding to 1024 bytes.
// Triggered by word-clock interrupt on GPIO 27 (codec I2S WS), divided by 32
// to produce ~1378 Hz block rate matching the P4's 44100/32 buffer cadence.

#include "p4_spi_transport.h"
#include "pico_audio_bridge.h"
#include "SpiProtocol.h"

#include "hardware/spi.h"
#include "hardware/gpio.h"
#include "hardware/dma.h"
#include "pico/stdlib.h"
#include <string.h>
#include <stdio.h>

// ── Pin assignments (Link1: spi1rp ↔ SPI2p4, high-speed audio) ────────
// On RP2350, GPIO 28-31 are SPI1 alternate function:
//   GPIO 28 = SPI1 RX  (MISO — from P4)
//   GPIO 29 = SPI1 CSn (manual GPIO — directly controlled)
//   GPIO 30 = SPI1 SCK (clock)
//   GPIO 31 = SPI1 TX  (MOSI — to P4)
// This is separate from spi0 (SD card on GPIO 2-7), no conflict.
#define P4_SPI       spi1
#define P4_CLK_PIN   30   // SPI1 SCK
#define P4_MOSI_PIN  31   // SPI1 TX
#define P4_CS_PIN    29   // SPI1 CSn pad, driven as manual GPIO
#define P4_MISO_PIN  28   // SPI1 RX
#define P4_RDY_PIN   22   // P4 ready-to-receive signal (active high)
#define P4_WS_PIN    27   // Codec word-clock (44100 Hz), used for sync

#define P4_SPI_FREQ  30000000  // 30 MHz

// Must match P4 firmware SPI_BUFFER_LEN
#define SPI_BUF_LEN  1024

// ── Double-buffered SPI transfer ───────────────────────────────────────
static uint8_t tx_buf[2][SPI_BUF_LEN];
static uint8_t rx_buf[2][SPI_BUF_LEN];
static uint32_t cur_buf = 0;

// ── Word-clock sync counter (ISR increments, poll drains) ──────────────
static volatile uint32_t ws_block_counter = 0;

// ── Protocol sequence counter (100-199, wrapping) ──────────────────────
static uint8_t seq_counter = 100;

// ── DMA channel for non-blocking SPI ───────────────────────────────────
static int dma_tx_chan = -1;
static int dma_rx_chan = -1;

// ── Word-clock ISR: count every 32nd edge = one audio block ────────────
static void ws_isr_callback(uint gpio, uint32_t events) {
    (void)gpio; (void)events;
    static uint32_t edge_count = 0;
    if (++edge_count >= 32) {
        edge_count = 0;
        ws_block_counter++;
    }
}

void p4_spi_transport_init(void) {
    // ── SPI1 peripheral ────────────────────────────────────────────────
    spi_init(P4_SPI, P4_SPI_FREQ);
    spi_set_format(P4_SPI, 8, SPI_CPOL_0, SPI_CPHA_0, SPI_MSB_FIRST);

    gpio_set_function(P4_CLK_PIN,  GPIO_FUNC_SPI);
    gpio_set_function(P4_MOSI_PIN, GPIO_FUNC_SPI);
    gpio_set_function(P4_MISO_PIN, GPIO_FUNC_SPI);

    // CS is manual (active low)
    gpio_init(P4_CS_PIN);
    gpio_set_dir(P4_CS_PIN, GPIO_OUT);
    gpio_put(P4_CS_PIN, 1);

    // RDY input (active high from P4)
    gpio_init(P4_RDY_PIN);
    gpio_set_dir(P4_RDY_PIN, GPIO_IN);
    gpio_pull_down(P4_RDY_PIN);

    // Word-clock sync input (GPIO 27 = I2S WS from P4 codec)
    gpio_init(P4_WS_PIN);
    gpio_set_dir(P4_WS_PIN, GPIO_IN);
    gpio_pull_down(P4_WS_PIN);
    gpio_set_irq_enabled_with_callback(P4_WS_PIN, GPIO_IRQ_EDGE_FALL,
                                       true, ws_isr_callback);

    // ── DMA channels for full-duplex SPI ───────────────────────────────
    dma_tx_chan = dma_claim_unused_channel(true);
    dma_rx_chan = dma_claim_unused_channel(true);

    // Clear buffers
    memset(tx_buf, 0, sizeof(tx_buf));
    memset(rx_buf, 0, sizeof(rx_buf));

    // Fingerprint in both buffers
    tx_buf[0][0] = 0xCA; tx_buf[0][1] = 0xFE;
    tx_buf[1][0] = 0xCA; tx_buf[1][1] = 0xFE;

    ws_block_counter = 0;

    printf("[P4-SPI] Transport initialized (spi1 @ %u MHz, GPIO %d/%d/%d/%d)\n",
           P4_SPI_FREQ / 1000000, P4_CLK_PIN, P4_MOSI_PIN, P4_MISO_PIN, P4_CS_PIN);
}

// ── Pack one frame into the current tx buffer ──────────────────────────
static void pack_frame(uint8_t *buf) {
    // buf[0..1] already = 0xCA 0xFE (fingerprint)
    uint8_t *base = buf + 2;

    struct p4_spi_request_header *hdr = (struct p4_spi_request_header *)base;
    struct p4_spi_request2 *req = (struct p4_spi_request2 *)(base + sizeof(struct p4_spi_request_header));

    memset(hdr, 0, sizeof(*hdr));
    memset(req, 0, sizeof(*req));

    // Payload
    req->magic  = 0xFEEDC0DE;
    req->magic2 = 0xFEEDC0DE;
    req->synth_midi_length = pab_pack_spi(req->synth_midi, sizeof(req->synth_midi));

    // Header
    hdr->magic = 0xCAFE;
    hdr->request_sequence_counter = seq_counter;
    seq_counter = 100 + ((seq_counter - 100 + 1) % 100);
    hdr->payload_length = sizeof(struct p4_spi_request2);

    // CRC: init=42, sum all payload bytes
    const uint8_t *payload = (const uint8_t *)req;
    uint16_t crc = 42;
    for (uint16_t i = 0; i < sizeof(struct p4_spi_request2); i++) {
        crc += payload[i];
    }
    hdr->payload_crc = crc;
}

// ── Blocking full-duplex SPI transfer via DMA ──────────────────────────
static void spi_transfer_dma(const uint8_t *tx, uint8_t *rx, size_t len) {
    // Skip transfer entirely if P4 is not ready (avoids hang when P4 is off)
    if (!gpio_get(P4_RDY_PIN)) return;

    gpio_put(P4_CS_PIN, 0);

    // RX DMA channel
    dma_channel_config rc = dma_channel_get_default_config(dma_rx_chan);
    channel_config_set_transfer_data_size(&rc, DMA_SIZE_8);
    channel_config_set_dreq(&rc, spi_get_dreq(P4_SPI, false));
    channel_config_set_read_increment(&rc, false);
    channel_config_set_write_increment(&rc, true);
    dma_channel_configure(dma_rx_chan, &rc,
        rx, &spi_get_hw(P4_SPI)->dr, len, false);

    // TX DMA channel
    dma_channel_config tc = dma_channel_get_default_config(dma_tx_chan);
    channel_config_set_transfer_data_size(&tc, DMA_SIZE_8);
    channel_config_set_dreq(&tc, spi_get_dreq(P4_SPI, true));
    channel_config_set_read_increment(&tc, true);
    channel_config_set_write_increment(&tc, false);
    dma_channel_configure(dma_tx_chan, &tc,
        &spi_get_hw(P4_SPI)->dr, tx, len, false);

    // Start both channels
    dma_start_channel_mask((1u << dma_tx_chan) | (1u << dma_rx_chan));

    // Wait for completion
    dma_channel_wait_for_finish_blocking(dma_tx_chan);
    dma_channel_wait_for_finish_blocking(dma_rx_chan);

    // Wait for SPI to drain
    while (spi_get_hw(P4_SPI)->sr & SPI_SSPSR_BSY_BITS)
        tight_loop_contents();

    gpio_put(P4_CS_PIN, 1);
}

void p4_spi_transport_poll(void) {
    if (ws_block_counter == 0) return;
    ws_block_counter = 0;  // consume (coalesce if > 1)

    // Pack PCM frame into current buffer
    pack_frame(tx_buf[cur_buf]);

    // Full-duplex DMA transfer
    spi_transfer_dma(tx_buf[cur_buf], rx_buf[cur_buf], SPI_BUF_LEN);

    // Swap double buffer
    cur_buf ^= 1;
}
