// P4 SPI Transport — blocking DMA audio sender (matching DaDa_SPI exactly)
// Link1 (spi1rp ↔ SPI2p4): high-speed audio over spi1 (GPIO 28-31) to the P4.
// Independent of spi0 (SD card / control link).
//
// This is a direct-register translation of the DaDa_SPI library from
// tbd-pico-seq3, which is the PROVEN WORKING implementation.
//
// DaDa_SPI::TransferBlockingDelayed() sequence (replicated exactly):
//   1. WaitUntilDMADoneBlocking()  — poll CTRL_TRIG.BUSY on TX and RX
//   2. WaitUntilP4IsReady()        — poll handshake GPIO (active HIGH)
//   3. StartDMA(tx, rx, len)       — configure TX+RX channels, trigger both
//   4. WaitUntilDMADoneBlocking()  — poll until both complete
//   5. busy_wait_us_32(15)         — 15 µs post-transfer delay
//
// CRITICAL WORKAROUND: On RP2350 with PSRAM, including stdbool.h in the wrong
// order causes PSRAM init to hang. To avoid this, we DON'T include any pico SDK
// headers. Instead we use direct register access (same as p4_control_link.c).

#include <stdio.h>       // MUST be first and alone — avoids PSRAM hang
#include <string.h>
#include <stdint.h>

#include "p4_spi_transport.h"
#include "SpiProtocol.h"

// Forward-declare pab_pack_spi instead of including pico_audio_bridge.h,
// because that header includes stdbool.h and pico/audio_i2s.h which pull
// in SDK headers that trigger the PSRAM init hang.
extern uint32_t pab_pack_spi(uint8_t *synth_midi_buf, uint32_t buf_size);

// ═══════════════════════════════════════════════════════════════════════
// Direct register definitions for RP2350B — NO SDK headers needed
// ═══════════════════════════════════════════════════════════════════════

// Base addresses (from RP2350 datasheet / addressmap.h)
#define SPI1_BASE           0x40088000
#define DMA_BASE            0x50000000
#define SIO_BASE            0xd0000000
#define PADS_BANK0_BASE     0x40038000  // Covers GPIO 0-47 on RP2350B
#define IO_BANK0_BASE       0x40028000  // Covers GPIO 0-47 on RP2350B
#define TIMER0_BASE         0x400b0000
#define RESETS_BASE         0x40020000

// Register access macro
#define REG32(base, off)    (*(volatile uint32_t *)((base) + (off)))

// SPI registers (PL022, same layout as RP2040)
#define SPI_SSPCR0          0x00
#define SPI_SSPCR1          0x04
#define SPI_SSPDR           0x08
#define SPI_SSPSR           0x0c
#define SPI_SSPCPSR         0x10
#define SPI_SSPDMACR        0x24    // DMA control register

// SPI status register bits
#define SPI_SR_TNF          (1u << 1)   // TX FIFO not full
#define SPI_SR_RNE          (1u << 2)   // RX FIFO not empty
#define SPI_SR_BSY          (1u << 4)   // SPI busy

// GPIO pad and function mux
#define PADS_GPIO_OFFSET(n) (4 + (n) * 4)
#define IO_GPIO_CTRL_OFFSET(n) (4 + (n) * 8)
#define FUNC_SPI            1
#define FUNC_SIO            5

// SIO GPIO registers (RP2350B: GPIO 0-31 normal, 32-47 use HI)
#define SIO_GPIO_IN         0x004
#define SIO_GPIO_OE_CLR     0x040

// Timer
#define TIMER_TIMERAWL      0x28

// Resets (to bring SPI1 out of reset)
#define RESETS_RESET         0x00
#define RESETS_RESET_DONE    0x08
#define RESETS_SPI1_BIT      (1u << 19)
#define RESETS_ATOMIC_CLR    0x3000

// ── DMA register layout (RP2350 datasheet §2.5) ───────────────────────
// Each DMA channel occupies 0x40 bytes (16 registers × 4 bytes)
#define DMA_CH_STRIDE       0x40
// Per-channel register offsets within each channel's 0x40 block
#define DMA_CH_READ_ADDR    0x00   // READ_ADDR
#define DMA_CH_WRITE_ADDR   0x04   // WRITE_ADDR
#define DMA_CH_TRANS_COUNT  0x08   // TRANS_COUNT
#define DMA_CH_CTRL_TRIG    0x0c   // CTRL / CTRL_TRIG (writing here triggers)
#define DMA_CH_CTRL         0x10   // AL1_CTRL (write without triggering)
// Global DMA register: write a channel bitmask to start multiple channels
#define DMA_MULTI_CHAN_TRIGGER 0x450    // RP2350 offset (0x430 on RP2040)

// CTRL_TRIG register bit layout (RP2350 datasheet §2.5.7)
#define DMA_CTRL_EN             (1u << 0)    // Channel enable
#define DMA_CTRL_DATA_SIZE_BYTE (0u << 2)    // 8-bit transfers (bits [3:2] = 00)
#define DMA_CTRL_INCR_READ      (1u << 4)    // Increment read address (bit 4)
#define DMA_CTRL_INCR_WRITE     (1u << 6)    // Increment write address (bit 6 on RP2350!)
#define DMA_CTRL_BUSY           (1u << 26)   // Channel is busy (read-only, bit 26 on RP2350)
// CHAIN_TO field: bits [16:13] on RP2350, set to own channel to disable chaining
#define DMA_CTRL_CHAIN_TO(n)    (((uint32_t)(n)) << 13)
// TREQ_SEL field: bits [22:17] on RP2350 (NOT [20:15] like RP2040!)
#define DMA_CTRL_TREQ_SEL(n)    (((uint32_t)(n)) << 17)
// DREQ numbers for SPI1 on RP2350 (from dreq.h)
#define DREQ_SPI1_TX            26
#define DREQ_SPI1_RX            27

// ── Pin assignments (Link1: spi1rp ↔ SPI2p4, high-speed audio) ────────
// Matches DaDa_SPI real_time_spi {spi1, 29, 31, 28, 30, 22, 30000000}
//   DaDa_SPI ctor: (spi, cs, mosi, miso, clk, handshake, speed)
#define P4_CLK_PIN   30   // SPI1 SCK
#define P4_MOSI_PIN  31   // SPI1 TX  (MOSI — to P4)
#define P4_CS_PIN    29   // SPI1 CSn (hardware-managed via GPIO_FUNC_SPI)
#define P4_MISO_PIN  28   // SPI1 RX  (MISO — from P4)
#define P4_RDY_PIN   22   // P4 handshake (active high = transaction queued)

// P4 rp2350_spi_stream STREAM_BUFFER_SIZE_ = 512 — MUST match exactly!
#define SPI_BUF_LEN  512

// ── TIMER1 alarm registers (use TIMER1 to avoid SDK alarm pool conflicts) ──
#define TIMER1_BASE         0x400b8000
#define TIMER_ALARM0        0x10
#define TIMER_ARMED         0x20
// TIMER_TIMERAWL already defined as 0x28
#define TIMER_INTR          0x3c
#define TIMER_INTE          0x40

// Alarm period: poll PWM counter + RDY at 5000 Hz.
// Codec frame rate = 44100/32 = 1378 Hz (725 µs). At 200 µs we detect
// each new codec frame within one ISR tick and respond immediately.
// ISR cost: ~12 µs per fire × 5000 = 6% CPU (most ticks exit early).
#define ALARM_PERIOD_US     200

// TIMER1 reset bit (bit 24 in RESETS register)
#define RESETS_TIMER1_BIT   (1u << 24)

// ── NVIC registers (ARM Cortex-M33 standard, per-core) ────────────────
#define NVIC_ISER_ADDR      0xE000E100   // Interrupt Set-Enable
#define NVIC_ICPR_ADDR      0xE000E280   // Interrupt Clear-Pending
#define VTOR_ADDR           0xE000ED08   // Vector Table Offset Register

// TIMER1_IRQ_0 on RP2350 = IRQ 4
#define TIMER1_IRQ_NUM      4

// ── TICKS generator (must enable for TIMER1 to count at 1 µs) ─────────
#define TICKS_BASE              0x40108000
#define TICKS_TIMER0_CYCLES     0x1c    // Already configured by SDK
#define TICKS_TIMER1_CTRL       0x24
#define TICKS_TIMER1_CYCLES     0x28

// ── PWM registers (hardware edge counter for word-clock sync) ─────────
// GPIO 27 carries the P4 codec I2S WS signal at 44100 Hz.
// PWM slice 1 counts falling edges in hardware — no CPU overhead.
// Every 32 edges = one codec DMA frame (44100/32 = 1378.125 Hz).
#define PWM_BASE            0x400A8000
#define PWM_CH_STRIDE       0x14    // 5 registers × 4 bytes per slice
#define PWM_CHn_CSR(n)      ((n) * PWM_CH_STRIDE + 0x00)
#define PWM_CHn_DIV(n)      ((n) * PWM_CH_STRIDE + 0x04)
#define PWM_CHn_CTR(n)      ((n) * PWM_CH_STRIDE + 0x08)
#define PWM_CHn_TOP(n)      ((n) * PWM_CH_STRIDE + 0x10)
// GPIO 27 → PWM slice (27>>1)&7 = 5, channel B  (RP2350B: GPIO<32 uses &7)
#define WS_PWM_SLICE        5
#define WS_PIN              27      // Codec word-clock input
#define WS_EDGES_PER_FRAME  32      // 32 WS cycles = 1 codec DMA buffer
#define FUNC_PWM            4       // GPIO function mux for PWM
#define RESETS_PWM_BIT      (1u << 16)

// ── SPI buffers ────────────────────────────────────────────────────────
static uint8_t tx_buf[SPI_BUF_LEN];
static uint8_t rx_buf[SPI_BUF_LEN];

// ── DMA channels (hard-assigned, no SDK claim system) ──────────────────
// Use channels 4 and 5 to avoid collisions with other peripherals.
#define DMA_TX_CHAN  4
#define DMA_RX_CHAN  5

// Pre-built CTRL values (set once at init, written every transfer via AL1_CTRL)
static uint32_t dma_tx_ctrl = 0;
static uint32_t dma_rx_ctrl = 0;

// ── Debug counters ─────────────────────────────────────────────────────
static volatile uint32_t dbg_frames_sent  = 0;
static volatile uint32_t dbg_isr_fires    = 0;   // total ISR invocations
static volatile uint32_t dbg_isr_dma_busy = 0;   // skipped: DMA still running
static volatile uint32_t dbg_isr_rdy_low  = 0;   // skipped: P4 not ready
static volatile uint32_t dbg_ws_skipped   = 0;   // word-clock frames we couldn't send
static uint32_t dbg_polls        = 0;

// ── Word-clock frame tracking (PWM hardware edge counter) ──────────────
static uint16_t ws_last_ctr    = 0;    // last PWM counter reading
static uint16_t ws_edge_accum  = 0;    // accumulated edges since last frame
static volatile uint32_t ws_frame_count  = 0;  // codec frames elapsed (from WS)
static volatile uint32_t sent_frame_count = 0; // SPI frames we've actually sent

// ── Protocol sequence counter (100-199, wrapping) ──────────────────────
static uint8_t seq_counter = 100;

// ═══════════════════════════════════════════════════════════════════════
// Direct register helper functions (no SDK, same pattern as p4_control_link.c)
// ═══════════════════════════════════════════════════════════════════════

static inline uint32_t get_time_us(void) {
    return REG32(TIMER0_BASE, TIMER_TIMERAWL);
}

static inline int gpio_read_pin(uint32_t pin) {
    return (REG32(SIO_BASE, SIO_GPIO_IN) >> pin) & 1;
}

static void gpio_set_spi_function(uint32_t pin) {
    // Configure pad: IE=1, DRIVE=4mA, SCHMITT=1 (match reset defaults 0x56 minus PDE)
    REG32(PADS_BANK0_BASE, PADS_GPIO_OFFSET(pin)) = (1u << 6) | (1u << 4) | (1u << 1);
    // Set function mux to SPI
    REG32(IO_BANK0_BASE, IO_GPIO_CTRL_OFFSET(pin)) = FUNC_SPI;
}

static void gpio_set_input_pulldown(uint32_t pin) {
    // Pad: IE=1, PDE=1 (pull-down), SCHMITT=1 for clean edges
    REG32(PADS_BANK0_BASE, PADS_GPIO_OFFSET(pin)) = (1u << 6) | (1u << 2) | (1u << 1);
    // Set function to SIO (GPIO)
    REG32(IO_BANK0_BASE, IO_GPIO_CTRL_OFFSET(pin)) = FUNC_SIO;
    // Clear output enable (make it input)
    REG32(SIO_BASE, SIO_GPIO_OE_CLR) = (1u << pin);
}

// ── DMA helpers (matching DaDa_SPI::IsBusy / WaitUntilDMADoneBlocking) ─
static inline int dma_is_busy(int chan) {
    return (REG32(DMA_BASE, chan * DMA_CH_STRIDE + DMA_CH_CTRL_TRIG) & DMA_CTRL_BUSY) != 0;
}

static void spi1_unreset(void) {
    // Clear SPI1 reset bit (take out of reset)
    REG32(RESETS_BASE + RESETS_ATOMIC_CLR, RESETS_RESET) = RESETS_SPI1_BIT;
    // Wait for SPI1 to come out of reset
    while (!(REG32(RESETS_BASE, RESETS_RESET_DONE) & RESETS_SPI1_BIT)) {}
}

// ── StartDMA (matches DaDa_SPI::StartDMA exactly) ──────────────────────
// DaDa_SPI does:
//   dma_channel_configure(tx, &ctx, &spi->dr, tx_buf, len, false);  // no start
//   dma_channel_configure(rx, &crx, rx_buf, &spi->dr, len, false);  // no start
//   dma_start_channel_mask((1u << tx) | (1u << rx));                 // start both
//
// Translated to direct register access:
static void start_dma(uint8_t *tx, uint8_t *rx, uint32_t len) {
    // ── TX channel: read from tx buffer, write to SPI1 DR ──────────────
    REG32(DMA_BASE, DMA_TX_CHAN * DMA_CH_STRIDE + DMA_CH_READ_ADDR)   = (uint32_t)(uintptr_t)tx;
    REG32(DMA_BASE, DMA_TX_CHAN * DMA_CH_STRIDE + DMA_CH_WRITE_ADDR)  = SPI1_BASE + SPI_SSPDR;
    REG32(DMA_BASE, DMA_TX_CHAN * DMA_CH_STRIDE + DMA_CH_TRANS_COUNT) = len;
    // Write CTRL (AL1_CTRL) WITHOUT triggering — sets config but doesn't start
    REG32(DMA_BASE, DMA_TX_CHAN * DMA_CH_STRIDE + DMA_CH_CTRL)        = dma_tx_ctrl;

    // ── RX channel: read from SPI1 DR, write to rx buffer ──────────────
    REG32(DMA_BASE, DMA_RX_CHAN * DMA_CH_STRIDE + DMA_CH_READ_ADDR)   = SPI1_BASE + SPI_SSPDR;
    REG32(DMA_BASE, DMA_RX_CHAN * DMA_CH_STRIDE + DMA_CH_WRITE_ADDR)  = (uint32_t)(uintptr_t)rx;
    REG32(DMA_BASE, DMA_RX_CHAN * DMA_CH_STRIDE + DMA_CH_TRANS_COUNT) = len;
    // Write CTRL (AL1_CTRL) WITHOUT triggering
    REG32(DMA_BASE, DMA_RX_CHAN * DMA_CH_STRIDE + DMA_CH_CTRL)        = dma_rx_ctrl;

    // ── Start both channels simultaneously ─────────────────────────────
    // DaDa_SPI: dma_start_channel_mask((1u << tx) | (1u << rx))
    REG32(DMA_BASE, DMA_MULTI_CHAN_TRIGGER) = (1u << DMA_TX_CHAN) | (1u << DMA_RX_CHAN);
}

// ── Pack one frame into tx_buf ─────────────────────────────────────────
static void pack_frame(void) {
    // tx_buf[0..1] = 0xCA 0xFE (set once at init, never changes)
    uint8_t *base = tx_buf + 2;

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

// ── Timer1 Alarm0 ISR — word-clock-paced SPI frame sender ──────────────
// Fires at 5000 Hz to poll the PWM edge counter on GPIO 27 (codec WS).
// Only sends one SPI frame per 32 WS edges (= one codec DMA cycle).
// This locks RP2350 output exactly to the P4 codec sample rate.
static void timer1_alarm0_isr(void) {
    // Clear the alarm interrupt (write 1 to bit 0)
    REG32(TIMER1_BASE, TIMER_INTR) = 1u;

    // Re-arm alarm for next tick (relative to now — no drift accumulation)
    uint32_t now = REG32(TIMER1_BASE, TIMER_TIMERAWL);
    REG32(TIMER1_BASE, TIMER_ALARM0) = now + ALARM_PERIOD_US;

    dbg_isr_fires++;

    // ── Read hardware edge counter (PWM slice 1 counts WS falling edges) ──
    uint16_t ctr = (uint16_t)REG32(PWM_BASE, PWM_CHn_CTR(WS_PWM_SLICE));
    uint16_t delta = ctr - ws_last_ctr;   // uint16 subtraction handles wrap
    ws_last_ctr = ctr;

    // Accumulate edges, convert to codec frames
    ws_edge_accum += delta;
    while (ws_edge_accum >= WS_EDGES_PER_FRAME) {
        ws_edge_accum -= WS_EDGES_PER_FRAME;
        ws_frame_count++;
    }

    // ── Only send if a new codec frame has occurred ────────────────────
    if (ws_frame_count <= sent_frame_count)
        return;

    // If previous DMA still running, skip this tick (will retry next tick)
    if (dma_is_busy(DMA_TX_CHAN) || dma_is_busy(DMA_RX_CHAN)) {
        dbg_isr_dma_busy++;
        return;
    }

    // If P4 not ready (handshake LOW), skip
    if (!gpio_read_pin(P4_RDY_PIN)) {
        dbg_isr_rdy_low++;
        return;
    }

    // Pack and send one frame (non-blocking — DMA completes in background)
    pack_frame();
    start_dma(tx_buf, rx_buf, SPI_BUF_LEN);

    // Snap to current frame count (don't burst-send if we fell behind)
    if (ws_frame_count > sent_frame_count + 1) {
        dbg_ws_skipped += (ws_frame_count - sent_frame_count - 1);
    }
    sent_frame_count = ws_frame_count;
    dbg_frames_sent++;
}

// ── Word-clock hardware counter init ───────────────────────────────────
// Configures PWM slice 1 to count falling edges on GPIO 27 (WS pin).
// This gives us a free-running hardware counter of codec WS cycles.
// The ISR reads this counter to determine when to send SPI frames.
static void ws_counter_init(void) {
    // Bring PWM out of reset
    REG32(RESETS_BASE + RESETS_ATOMIC_CLR, RESETS_RESET) = RESETS_PWM_BIT;
    while (!(REG32(RESETS_BASE, RESETS_RESET_DONE) & RESETS_PWM_BIT)) {}

    // Configure GPIO 27 for PWM function (edge counting on channel B)
    // Pad: IE=1 (bit 6), SCHMITT=1 (bit 1) for clean edges
    REG32(PADS_BANK0_BASE, PADS_GPIO_OFFSET(WS_PIN)) = (1u << 6) | (1u << 1);
    REG32(IO_BANK0_BASE, IO_GPIO_CTRL_OFFSET(WS_PIN)) = FUNC_PWM;

    // Configure PWM slice 1 for falling-edge counting on B pin
    REG32(PWM_BASE, PWM_CHn_CSR(WS_PWM_SLICE)) = 0;            // Disable first
    REG32(PWM_BASE, PWM_CHn_DIV(WS_PWM_SLICE)) = 1u << 4;      // DIV=1 (INT=1, FRAC=0)
    REG32(PWM_BASE, PWM_CHn_CTR(WS_PWM_SLICE)) = 0;            // Reset counter
    REG32(PWM_BASE, PWM_CHn_TOP(WS_PWM_SLICE)) = 0xFFFF;       // Free-running 16-bit
    // CSR: DIVMODE=0b11 (falling edge on B pin), EN=1
    REG32(PWM_BASE, PWM_CHn_CSR(WS_PWM_SLICE)) = (3u << 4) | 1u;

    ws_last_ctr = 0;
    ws_edge_accum = 0;
    ws_frame_count = 0;
    sent_frame_count = 0;

    printf("[P4-SPI] Word-clock counter: PWM slice %d on GPIO %d, %d edges/frame\n",
           WS_PWM_SLICE, WS_PIN, WS_EDGES_PER_FRAME);
}

void p4_spi_transport_init(void) {
    printf("[P4-SPI] init...\n");

    // ── Bring SPI1 out of reset ────────────────────────────────────────
    spi1_unreset();

    // ── Configure SPI1: Mode 3 (CPOL=1, CPHA=1), 8-bit ────────────────
    // Disable SPI before configuring
    REG32(SPI1_BASE, SPI_SSPCR1) = 0;

    // SSPCR0: SCR=0 (bits 15:8), SPH=1 (bit 7), SPO=1 (bit 6), FRF=00, DSS=0111 (8-bit)
    // Matches DaDa_SPI: spi_set_format(spi1, 8, SPI_CPOL_1, SPI_CPHA_1, SPI_MSB_FIRST)
    REG32(SPI1_BASE, SPI_SSPCR0)  = (1u << 7) | (1u << 6) | 0x07;

    // SSPCPSR: clock prescaler
    // SDK spi_init(spi1, 30000000) with 150 MHz sys_clk:
    //   prescale=2, postdiv=3 → 150/(2*3) = 25 MHz (closest to 30 MHz)
    //   SSPCPSR=2, SSPCR0.SCR=2
    // We use SSPCPSR=6, SCR=0 → 150/6 = 25 MHz (same result, simpler)
    REG32(SPI1_BASE, SPI_SSPCPSR) = 6;

    // Drain any stale data from RX FIFO before enabling
    while (REG32(SPI1_BASE, SPI_SSPSR) & SPI_SR_RNE) {
        (void)REG32(SPI1_BASE, SPI_SSPDR);
    }

    // Enable SPI (SSE=1)
    REG32(SPI1_BASE, SPI_SSPCR1) = (1u << 1);

    // Enable SPI DMA requests (TXDMAE=1, RXDMAE=1)
    REG32(SPI1_BASE, SPI_SSPDMACR) = (1u << 0) | (1u << 1);

    // ── Configure GPIO pins for SPI1 function ──────────────────────────
    // ALL 4 pins set to SPI function (hardware-managed CS)
    // Matches DaDa_SPI constructor exactly.
    gpio_set_spi_function(P4_CLK_PIN);    // 30 = SPI1 SCK
    gpio_set_spi_function(P4_MOSI_PIN);   // 31 = SPI1 TX
    gpio_set_spi_function(P4_MISO_PIN);   // 28 = SPI1 RX
    gpio_set_spi_function(P4_CS_PIN);     // 29 = SPI1 CSn (hardware-managed)

    // ── Handshake input (active high from P4 = transaction queued) ─────
    // Matches DaDa_SPI: gpio_init(handshake), gpio_set_dir(IN), gpio_pull_down()
    gpio_set_input_pulldown(P4_RDY_PIN);  // 22 = P4 RDY

    // ── Initialize word-clock edge counter on GPIO 27 ──────────────────
    ws_counter_init();

    // ── Build DMA CTRL values ──────────────────────────────────────────
    // These match DaDa_SPI's channel_config_set_* calls exactly:
    //
    // TX config: DMA_SIZE_8, DREQ=SPI1_TX, INCR_READ=true, INCR_WRITE=false
    // CHAIN_TO=self disables chaining (otherwise defaults to ch0 = spurious trigger)
    dma_tx_ctrl = DMA_CTRL_EN
                | DMA_CTRL_DATA_SIZE_BYTE
                | DMA_CTRL_INCR_READ
                | DMA_CTRL_TREQ_SEL(DREQ_SPI1_TX)
                | DMA_CTRL_CHAIN_TO(DMA_TX_CHAN);

    // RX config: DMA_SIZE_8, DREQ=SPI1_RX, INCR_READ=false, INCR_WRITE=true
    dma_rx_ctrl = DMA_CTRL_EN
                | DMA_CTRL_DATA_SIZE_BYTE
                | DMA_CTRL_INCR_WRITE
                | DMA_CTRL_TREQ_SEL(DREQ_SPI1_RX)
                | DMA_CTRL_CHAIN_TO(DMA_RX_CHAN);

    // ── Clear buffers and stamp fingerprint ────────────────────────────
    memset(tx_buf, 0, sizeof(tx_buf));
    memset(rx_buf, 0, sizeof(rx_buf));
    tx_buf[0] = 0xCA;
    tx_buf[1] = 0xFE;

    printf("[P4-SPI] init OK (spi1, 25MHz, Mode3, DMA ch%d/%d, buf=%d, CS=%d, RDY=%d)\n",
           DMA_TX_CHAN, DMA_RX_CHAN, SPI_BUF_LEN, P4_CS_PIN, P4_RDY_PIN);
    printf("[P4-SPI] CTRL TX=0x%08lx RX=0x%08lx\n",
           (unsigned long)dma_tx_ctrl, (unsigned long)dma_rx_ctrl);
    printf("[P4-SPI] RDY pin %d reads: %d\n", P4_RDY_PIN, gpio_read_pin(P4_RDY_PIN));

    // ── Set up TIMER1 alarm for periodic frame pacing ──────────────────
    // Unreset TIMER1 (separate from TIMER0 used by SDK)
    REG32(RESETS_BASE + RESETS_ATOMIC_CLR, RESETS_RESET) = RESETS_TIMER1_BIT;
    while (!(REG32(RESETS_BASE, RESETS_RESET_DONE) & RESETS_TIMER1_BIT)) {}

    // Enable TIMER1 tick generator — SDK only enables TIMER0's tick.
    // Copy the same CYCLES value (12 for 12 MHz XOSC → 1 µs ticks).
    uint32_t tick_cycles = REG32(TICKS_BASE, TICKS_TIMER0_CYCLES) & 0x1FF;
    REG32(TICKS_BASE, TICKS_TIMER1_CYCLES) = tick_cycles;
    REG32(TICKS_BASE, TICKS_TIMER1_CTRL) = 1u;  // ENABLE

    printf("[P4-SPI] TIMER1 tick enabled (cycles=%lu)\n", (unsigned long)tick_cycles);

    // Install ISR in RAM vector table
    volatile uint32_t *vtable = (volatile uint32_t *)(*(volatile uint32_t *)VTOR_ADDR);
    vtable[16 + TIMER1_IRQ_NUM] = (uint32_t)(uintptr_t)timer1_alarm0_isr;

    // Clear any pending interrupt, then enable in NVIC
    *(volatile uint32_t *)NVIC_ICPR_ADDR = (1u << TIMER1_IRQ_NUM);
    *(volatile uint32_t *)NVIC_ISER_ADDR = (1u << TIMER1_IRQ_NUM);

    // Enable TIMER1 ALARM0 interrupt
    REG32(TIMER1_BASE, TIMER_INTE) = 1u;

    // Arm first alarm
    uint32_t t1_now = REG32(TIMER1_BASE, TIMER_TIMERAWL);
    REG32(TIMER1_BASE, TIMER_ALARM0) = t1_now + ALARM_PERIOD_US;

    printf("[P4-SPI] Timer1 alarm armed: %lu us period (~%lu Hz)\n",
           (unsigned long)ALARM_PERIOD_US, 1000000UL / ALARM_PERIOD_US);
}

void p4_spi_transport_poll(void) {
    // Frame sending is handled by TIMER1 alarm ISR at ~5000 Hz.
    // This function just prints periodic debug stats.
    if (++dbg_polls >= 70) {
        dbg_polls = 0;
        printf("[P4-SPI] sent=%lu isr=%lu dma_busy=%lu rdy_low=%lu ws_skip=%lu rdy=%d\n",
               (unsigned long)dbg_frames_sent,
               (unsigned long)dbg_isr_fires,
               (unsigned long)dbg_isr_dma_busy,
               (unsigned long)dbg_isr_rdy_low,
               (unsigned long)dbg_ws_skipped,
               gpio_read_pin(P4_RDY_PIN));
    }
}
