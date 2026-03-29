// tbd16_audio.c — TBD-16 audio driver implementation
// See tbd16_audio.h for API documentation.
//
// Self-contained: uses direct register access instead of Pico SDK headers
// to avoid the RP2350 PSRAM init hang (SDK headers before stdio.h).
// Safe to use in projects without PSRAM too.
//
// IMPORTANT: #include <stdio.h> MUST be first to avoid PSRAM init hang.
// Do NOT include any pico SDK hardware/*.h headers in this file.

#include <stdio.h>      // MUST be first — avoids PSRAM init hang on RP2350
#include <string.h>
#include <stdint.h>
#include "tbd16_audio.h"

// ═══════════════════════════════════════════════════════════════════════
// Register Definitions (direct access — no SDK headers)
// ═══════════════════════════════════════════════════════════════════════

#define REG32(base, off) (*(volatile uint32_t *)((base) + (off)))

// Peripheral bases
#define SPI0_BASE       0x40080000
#define SPI1_BASE       0x40088000
#define DMA_BASE        0x50000000
#define SIO_BASE        0xd0000000
#define PADS_BANK0_BASE 0x40038000
#define IO_BANK0_BASE   0x40028000
#define TIMER0_BASE     0x400b0000
#define TIMER1_BASE     0x400b8000
#define RESETS_BASE     0x40020000
#define PWM_BASE        0x400A8000
#define TICKS_BASE      0x40108000

// SPI registers
#define SPI_CR0         0x00
#define SPI_CR1         0x04
#define SPI_DR          0x08
#define SPI_SR          0x0c
#define SPI_CPSR        0x10
#define SPI_DMACR       0x24
#define SPI_SR_TNF      (1u << 1)
#define SPI_SR_RNE      (1u << 2)

// GPIO
#define PADS_OFF(n)     (4 + (n) * 4)
#define IO_CTRL_OFF(n)  (4 + (n) * 8)
#define FUNC_SPI        1
#define FUNC_SIO        5
#define FUNC_PWM        4

// SIO
#define SIO_IN          0x004
#define SIO_HI_IN       0x008
#define SIO_OE_CLR      0x040
#define SIO_HI_OE_CLR  0x054

// Timer
#define T_ALARM0        0x10
#define T_ARMED         0x20
#define T_RAWL          0x28
#define T_INTR          0x3c
#define T_INTE          0x40

// DMA (RP2350 bit positions — differ from RP2040!)
#define DMA_STRIDE      0x40
#define DMA_READ        0x00
#define DMA_WRITE       0x04
#define DMA_COUNT       0x08
#define DMA_CTRL_TRIG   0x0c
#define DMA_CTRL        0x10
#define DMA_MULTI_TRIG  0x450
#define DMA_EN          (1u << 0)
#define DMA_SIZE_8      (0u << 2)
#define DMA_INCR_RD     (1u << 4)
#define DMA_INCR_WR     (1u << 6)
#define DMA_BUSY        (1u << 26)
#define DMA_CHAIN(n)    (((uint32_t)(n)) << 13)
#define DMA_TREQ(n)     (((uint32_t)(n)) << 17)
#define DREQ_SPI1_TX    26
#define DREQ_SPI1_RX    27

// Resets
#define RST_RESET       0x00
#define RST_DONE        0x08
#define RST_ATOMIC_CLR  0x3000
#define RST_SPI0        (1u << 18)
#define RST_SPI1        (1u << 19)
#define RST_PWM         (1u << 16)
#define RST_TIMER1      (1u << 24)

// PWM
#define PWM_STRIDE      0x14
#define PWM_CSR(n)      ((n) * PWM_STRIDE + 0x00)
#define PWM_DIV(n)      ((n) * PWM_STRIDE + 0x04)
#define PWM_CTR(n)      ((n) * PWM_STRIDE + 0x08)
#define PWM_TOP(n)      ((n) * PWM_STRIDE + 0x10)

// NVIC
#define NVIC_ISER       0xE000E100
#define NVIC_ICPR       0xE000E280
#define VTOR            0xE000ED08

// Ticks
#define TICKS_T0_CYC    0x1c
#define TICKS_T1_CTRL   0x24
#define TICKS_T1_CYC    0x28

// ═══════════════════════════════════════════════════════════════════════
// Pin Assignments (fixed on TBD-16 PCB)
// ═══════════════════════════════════════════════════════════════════════

// Link1 — Audio (SPI1)
#define PIN_MISO        28
#define PIN_CS          29
#define PIN_CLK         30
#define PIN_MOSI        31
#define PIN_RDY         22  // P4 handshake (active high)
#define PIN_WS          27  // Codec word clock (44100 Hz)

// Link2 — Control (SPI0)
#define CTL_MISO        32
#define CTL_CS          33
#define CTL_CLK         34
#define CTL_MOSI        35
#define CTL_RDY         18  // P4 ready-for-command (active high)

// ═══════════════════════════════════════════════════════════════════════
// SPI Protocol Structures
// ═══════════════════════════════════════════════════════════════════════

#define SPI_BUF_LEN     512
#define PCM2_MAGIC      0x50434D32  // "PCM2"
#define SYNTH_MIDI_SIZE 256

struct __attribute__((packed)) req_header {
    uint16_t magic;           // 0xCAFE
    uint8_t  seq;             // 100-199
    uint8_t  reserved1[5];
    uint16_t payload_length;  // 276
    uint16_t payload_crc;
    uint32_t reserved2;
};

struct __attribute__((packed)) req_payload {
    uint32_t magic;           // 0xFEEDC0DE
    uint32_t synth_midi_length;
    uint8_t  synth_midi[SYNTH_MIDI_SIZE];
    uint32_t sequencer_tempo;
    uint32_t sequencer_active_track;
    uint32_t magic2;          // 0xFEEDC0DE
};

// ═══════════════════════════════════════════════════════════════════════
// Ring Buffer
// ═══════════════════════════════════════════════════════════════════════

#define RING_SIZE       TBD16_RING_SIZE
#define RING_MASK       (RING_SIZE - 1)
#define MIX_BUF_PAIRS   128
#define MAX_PER_FRAME   62

static int16_t ring[RING_SIZE * 2];          // stereo interleaved
static volatile uint32_t ring_wr = 0;
static volatile uint32_t ring_rd = 0;

static int16_t mix_pcm[MIX_BUF_PAIRS * 2];
static tbd16_audio_buffer_mem_t mix_mem;
static tbd16_audio_buffer_t mix_buf;

static inline uint32_t ring_count(void) {
    return (ring_wr - ring_rd) & RING_MASK;
}
static inline uint32_t ring_free(void) {
    return (RING_SIZE - 1) - ring_count();
}

// ═══════════════════════════════════════════════════════════════════════
// State
// ═══════════════════════════════════════════════════════════════════════

static uint32_t source_rate = 44100;
static uint32_t target_per_frame = 32;

static uint8_t tx_buf[SPI_BUF_LEN];
static uint8_t rx_buf[SPI_BUF_LEN];
static uint32_t dma_tx_ctrl_val = 0;
static uint32_t dma_rx_ctrl_val = 0;
static uint8_t seq_counter = 100;

#define DMA_TX  4
#define DMA_RX  5
#define WS_SLICE 5
#define WS_EDGES 32
#define ISR_PERIOD_US 200
#define TIMER1_IRQ 4

static uint16_t ws_last = 0;
static uint16_t ws_accum = 0;
static volatile uint32_t ws_frames = 0;
static volatile uint32_t sent_frames = 0;

// Callback mode
static tbd16_audio_callback_t user_callback = 0;
static int16_t cb_buf[MAX_PER_FRAME * 2];

// Test tone
static int test_tone_on = 0;
static uint32_t tt_phase = 0;
#define TT_STEP 42852277u  // 440 Hz at 44100 Hz

static const int16_t sine_qw[64] = {
        0,   499,   997,  1495,  1991,  2487,  2981,  3473,
     3963,  4450,  4935,  5417,  5895,  6370,  6840,  7307,
     7769,  8226,  8678,  9124,  9565, 10000, 10429, 10851,
    11266, 11675, 12076, 12470, 12856, 13234, 13603, 13965,
    14317, 14661, 14996, 15321, 15637, 15943, 16239, 16525,
    16801, 17066, 17321, 17564, 17797, 18019, 18230, 18430,
    18617, 18794, 18959, 19111, 19252, 19382, 19499, 19603,
    19696, 19777, 19845, 19901, 19944, 19975, 19994, 20000,
};

static inline int16_t sine256(uint8_t idx) {
    if (idx < 64)  return  sine_qw[idx];
    if (idx < 128) return  sine_qw[127 - idx];
    if (idx < 192) return -sine_qw[idx - 128];
    return                 -sine_qw[255 - idx];
}

// ═══════════════════════════════════════════════════════════════════════
// Low-Level Helpers
// ═══════════════════════════════════════════════════════════════════════

static inline uint32_t time_us(void) {
    return REG32(TIMER0_BASE, T_RAWL);
}

static inline void delay_us(uint32_t us) {
    uint32_t s = time_us();
    while (time_us() - s < us) {}
}

static inline int gpio_in(uint32_t pin) {
    if (pin < 32) return (REG32(SIO_BASE, SIO_IN) >> pin) & 1;
    return (REG32(SIO_BASE, SIO_HI_IN) >> (pin - 32)) & 1;
}

static void gpio_spi_func(uint32_t pin) {
    REG32(PADS_BANK0_BASE, PADS_OFF(pin)) = (1u << 6) | (1u << 4) | (1u << 1);
    REG32(IO_BANK0_BASE, IO_CTRL_OFF(pin)) = FUNC_SPI;
}

static void gpio_input_pd(uint32_t pin) {
    REG32(PADS_BANK0_BASE, PADS_OFF(pin)) = (1u << 6) | (1u << 2) | (1u << 1);
    REG32(IO_BANK0_BASE, IO_CTRL_OFF(pin)) = FUNC_SIO;
    if (pin < 32) REG32(SIO_BASE, SIO_OE_CLR) = (1u << pin);
    else REG32(SIO_BASE, SIO_HI_OE_CLR) = (1u << (pin - 32));
}

static inline int dma_busy(int ch) {
    return (REG32(DMA_BASE, ch * DMA_STRIDE + DMA_CTRL_TRIG) & DMA_BUSY) != 0;
}

static void unreset(uint32_t bit) {
    REG32(RESETS_BASE + RST_ATOMIC_CLR, RST_RESET) = bit;
    while (!(REG32(RESETS_BASE, RST_DONE) & bit)) {}
}

// ═══════════════════════════════════════════════════════════════════════
// DMA Transfer
// ═══════════════════════════════════════════════════════════════════════

static void start_dma(void) {
    REG32(DMA_BASE, DMA_TX * DMA_STRIDE + DMA_READ)  = (uint32_t)(uintptr_t)tx_buf;
    REG32(DMA_BASE, DMA_TX * DMA_STRIDE + DMA_WRITE) = SPI1_BASE + SPI_DR;
    REG32(DMA_BASE, DMA_TX * DMA_STRIDE + DMA_COUNT) = SPI_BUF_LEN;
    REG32(DMA_BASE, DMA_TX * DMA_STRIDE + DMA_CTRL)  = dma_tx_ctrl_val;

    REG32(DMA_BASE, DMA_RX * DMA_STRIDE + DMA_READ)  = SPI1_BASE + SPI_DR;
    REG32(DMA_BASE, DMA_RX * DMA_STRIDE + DMA_WRITE) = (uint32_t)(uintptr_t)rx_buf;
    REG32(DMA_BASE, DMA_RX * DMA_STRIDE + DMA_COUNT) = SPI_BUF_LEN;
    REG32(DMA_BASE, DMA_RX * DMA_STRIDE + DMA_CTRL)  = dma_rx_ctrl_val;

    REG32(DMA_BASE, DMA_MULTI_TRIG) = (1u << DMA_TX) | (1u << DMA_RX);
}

// ═══════════════════════════════════════════════════════════════════════
// PCM Packing
// ═══════════════════════════════════════════════════════════════════════

static uint32_t pack_pcm2(uint8_t *midi_buf, uint32_t buf_size) {
    // Test tone mode
    if (test_tone_on) {
        uint32_t count = 32;
        if (buf_size < 8 + count * 4) return 0;
        uint32_t magic = PCM2_MAGIC;
        uint16_t c16 = (uint16_t)count;
        uint16_t r16 = 44100;
        memcpy(midi_buf, &magic, 4);
        memcpy(midi_buf + 4, &c16, 2);
        memcpy(midi_buf + 6, &r16, 2);
        int16_t *out = (int16_t *)(midi_buf + 8);
        for (uint32_t i = 0; i < count; i++) {
            int16_t v = sine256((uint8_t)(tt_phase >> 24));
            out[i * 2] = v;
            out[i * 2 + 1] = v;
            tt_phase += TT_STEP;
        }
        return 8 + count * 4;
    }

    // Callback mode — generate samples on demand
    if (user_callback) {
        uint32_t count = target_per_frame;
        if (buf_size < 8 + count * 4) return 0;
        user_callback(cb_buf, count);
        uint32_t magic = PCM2_MAGIC;
        uint16_t c16 = (uint16_t)count;
        uint16_t r16 = (uint16_t)source_rate;
        memcpy(midi_buf, &magic, 4);
        memcpy(midi_buf + 4, &c16, 2);
        memcpy(midi_buf + 6, &r16, 2);
        memcpy(midi_buf + 8, cb_buf, count * 4);
        return 8 + count * 4;
    }

    // Ring buffer mode
    uint32_t avail = ring_count();
    uint32_t count = avail < target_per_frame ? avail : target_per_frame;
    if (count == 0) return 0;

    uint32_t payload = 8 + count * 4;
    if (buf_size < payload) return 0;

    int16_t *out = (int16_t *)(midi_buf + 8);
    uint32_t r = ring_rd;
    for (uint32_t i = 0; i < count; i++) {
        uint32_t idx = r & RING_MASK;
        out[i * 2]     = ring[idx * 2];
        out[i * 2 + 1] = ring[idx * 2 + 1];
        r++;
    }
    ring_rd = r & RING_MASK;

    uint32_t magic = PCM2_MAGIC;
    uint16_t c16 = (uint16_t)count;
    uint16_t r16 = (uint16_t)source_rate;
    memcpy(midi_buf, &magic, 4);
    memcpy(midi_buf + 4, &c16, 2);
    memcpy(midi_buf + 6, &r16, 2);
    return payload;
}

// ═══════════════════════════════════════════════════════════════════════
// Frame Packing
// ═══════════════════════════════════════════════════════════════════════

static void pack_frame(void) {
    uint8_t *base = tx_buf + 2;
    struct req_header *hdr = (struct req_header *)base;
    struct req_payload *req = (struct req_payload *)(base + sizeof(struct req_header));

    memset(hdr, 0, sizeof(*hdr));
    memset(req, 0, sizeof(*req));

    req->magic  = 0xFEEDC0DE;
    req->magic2 = 0xFEEDC0DE;
    req->synth_midi_length = pack_pcm2(req->synth_midi, SYNTH_MIDI_SIZE);

    hdr->magic = 0xCAFE;
    hdr->seq = seq_counter;
    seq_counter = 100 + ((seq_counter - 100 + 1) % 100);
    hdr->payload_length = sizeof(struct req_payload);

    const uint8_t *p = (const uint8_t *)req;
    uint16_t crc = 42;
    for (uint16_t i = 0; i < sizeof(struct req_payload); i++)
        crc += p[i];
    hdr->payload_crc = crc;
}

// ═══════════════════════════════════════════════════════════════════════
// TIMER1 ISR — Word-Clock-Paced Frame Sender
// ═══════════════════════════════════════════════════════════════════════

static void timer1_isr(void) {
    REG32(TIMER1_BASE, T_INTR) = 1u;
    uint32_t now = REG32(TIMER1_BASE, T_RAWL);
    REG32(TIMER1_BASE, T_ALARM0) = now + ISR_PERIOD_US;

    // Read hardware edge counter
    uint16_t ctr = (uint16_t)REG32(PWM_BASE, PWM_CTR(WS_SLICE));
    uint16_t delta = ctr - ws_last;
    ws_last = ctr;

    ws_accum += delta;
    while (ws_accum >= WS_EDGES) {
        ws_accum -= WS_EDGES;
        ws_frames++;
    }

    if (ws_frames <= sent_frames) return;
    if (dma_busy(DMA_TX) || dma_busy(DMA_RX)) return;
    if (!gpio_in(PIN_RDY)) return;

    pack_frame();
    start_dma();
    sent_frames = ws_frames;
}

// ═══════════════════════════════════════════════════════════════════════
// Control Link (SPI0) — SetActivePlugin
// ═══════════════════════════════════════════════════════════════════════

static uint8_t ctl_tx[2048];
static uint8_t ctl_rx[2048];

static void ctl_spi_xfer(const uint8_t *tx, uint8_t *rx, uint32_t len) {
    for (uint32_t i = 0; i < len; i++) {
        while (!(REG32(SPI0_BASE, SPI_SR) & SPI_SR_TNF)) {}
        REG32(SPI0_BASE, SPI_DR) = tx[i];
        while (!(REG32(SPI0_BASE, SPI_SR) & SPI_SR_RNE)) {}
        rx[i] = REG32(SPI0_BASE, SPI_DR) & 0xFF;
    }
}

static void activate_plugin(void) {
    // Bring SPI0 out of reset (may have been used for SD card)
    unreset(RST_SPI0);

    // SPI0: 25 MHz, Mode 3, 8-bit
    REG32(SPI0_BASE, SPI_CR1)  = 0;
    REG32(SPI0_BASE, SPI_CPSR) = 6;  // 150/6 = 25 MHz
    REG32(SPI0_BASE, SPI_CR0)  = (1u << 7) | (1u << 6) | 0x07;
    REG32(SPI0_BASE, SPI_CR1)  = (1u << 1);

    // GPIO 32-35 for SPI0
    for (int p = CTL_MISO; p <= CTL_MOSI; p++) {
        REG32(PADS_BANK0_BASE, PADS_OFF(p)) = (1u << 6);
        REG32(IO_BANK0_BASE, IO_CTRL_OFF(p)) = FUNC_SPI;
    }

    // Wait for P4 word-clock (confirms P4 is alive)
    printf("[TBD16] Waiting for P4 codec...\n");
    gpio_input_pd(PIN_WS);
    uint32_t pulses = 0;
    uint32_t deadline = time_us() + 5000000;  // 5 sec timeout
    int last = gpio_in(PIN_WS);
    while (pulses < 100) {
        if ((int32_t)(time_us() - deadline) >= 0) {
            printf("[TBD16] WARNING: P4 codec not detected\n");
            break;
        }
        int cur = gpio_in(PIN_WS);
        if (last && !cur) pulses++;
        last = cur;
    }
    printf("[TBD16] P4 alive (%lu WS pulses)\n", (unsigned long)pulses);

    // Wait for P4 control link ready
    deadline = time_us() + 100000;
    while (!gpio_in(CTL_RDY)) {
        if ((int32_t)(time_us() - deadline) >= 0) break;
    }

    // Send SetActivePlugin(0, "PicoAudioBridge")
    memset(ctl_tx, 0, sizeof(ctl_tx));
    ctl_tx[0] = 0xCA;
    ctl_tx[1] = 0xFE;
    ctl_tx[2] = 0x04;  // REQ_SET_ACTIVE_PLUGIN
    ctl_tx[3] = 0;     // channel 0
    const char *name = "PicoAudioBridge";
    int32_t nlen = (int32_t)strlen(name);
    memcpy(&ctl_tx[5], &nlen, 4);
    memcpy(&ctl_tx[9], name, (size_t)nlen + 1);

    ctl_spi_xfer(ctl_tx, ctl_rx, 2048);
    delay_us(15);

    if (ctl_rx[0] == 0xCA && ctl_rx[1] == 0xFE) {
        printf("[TBD16] PicoAudioBridge activated\n");
    } else {
        printf("[TBD16] WARNING: Plugin activation may have failed\n");
    }
    delay_us(10000);
}

// ═══════════════════════════════════════════════════════════════════════
// Public API
// ═══════════════════════════════════════════════════════════════════════

void tbd16_audio_init(uint32_t source_rate_hz) {
    source_rate = source_rate_hz;
    target_per_frame = (source_rate * 32u) / 44100u;
    if (target_per_frame < 1) target_per_frame = 1;
    if (target_per_frame > MAX_PER_FRAME) target_per_frame = MAX_PER_FRAME;

    // Init ring buffer
    ring_wr = 0;
    ring_rd = 0;
    memset(ring, 0, sizeof(ring));
    mix_mem.bytes = (uint8_t *)mix_pcm;
    mix_mem.size  = sizeof(mix_pcm);
    mix_buf.buffer = &mix_mem;
    mix_buf.max_sample_count = MIX_BUF_PAIRS;
    mix_buf.sample_count = 0;

    printf("[TBD16] Audio init (source=%lu Hz, %lu samples/frame)\n",
           (unsigned long)source_rate, (unsigned long)target_per_frame);

    // Activate PicoAudioBridge plugin on P4
    activate_plugin();

    // ── SPI1 init ──
    unreset(RST_SPI1);
    REG32(SPI1_BASE, SPI_CR1) = 0;
    REG32(SPI1_BASE, SPI_CR0) = (1u << 7) | (1u << 6) | 0x07;  // Mode 3, 8-bit
    REG32(SPI1_BASE, SPI_CPSR) = 6;  // 25 MHz
    while (REG32(SPI1_BASE, SPI_SR) & SPI_SR_RNE)
        (void)REG32(SPI1_BASE, SPI_DR);
    REG32(SPI1_BASE, SPI_CR1) = (1u << 1);
    REG32(SPI1_BASE, SPI_DMACR) = 3;  // TX + RX DMA enable

    // GPIO for SPI1
    gpio_spi_func(PIN_CLK);
    gpio_spi_func(PIN_MOSI);
    gpio_spi_func(PIN_MISO);
    gpio_spi_func(PIN_CS);
    gpio_input_pd(PIN_RDY);

    // ── Word-clock edge counter (PWM slice 5 on GPIO 27) ──
    unreset(RST_PWM);
    REG32(PADS_BANK0_BASE, PADS_OFF(PIN_WS)) = (1u << 6) | (1u << 1);
    REG32(IO_BANK0_BASE, IO_CTRL_OFF(PIN_WS)) = FUNC_PWM;
    REG32(PWM_BASE, PWM_CSR(WS_SLICE)) = 0;
    REG32(PWM_BASE, PWM_DIV(WS_SLICE)) = 1u << 4;
    REG32(PWM_BASE, PWM_CTR(WS_SLICE)) = 0;
    REG32(PWM_BASE, PWM_TOP(WS_SLICE)) = 0xFFFF;
    REG32(PWM_BASE, PWM_CSR(WS_SLICE)) = (3u << 4) | 1u;  // Falling edge B, enable

    // ── DMA control values ──
    dma_tx_ctrl_val = DMA_EN | DMA_SIZE_8 | DMA_INCR_RD
                    | DMA_TREQ(DREQ_SPI1_TX) | DMA_CHAIN(DMA_TX);
    dma_rx_ctrl_val = DMA_EN | DMA_SIZE_8 | DMA_INCR_WR
                    | DMA_TREQ(DREQ_SPI1_RX) | DMA_CHAIN(DMA_RX);

    // ── TX buffer fingerprint ──
    memset(tx_buf, 0, sizeof(tx_buf));
    tx_buf[0] = 0xCA;
    tx_buf[1] = 0xFE;

    // ── TIMER1 alarm ISR ──
    unreset(RST_TIMER1);
    uint32_t cyc = REG32(TICKS_BASE, TICKS_T0_CYC) & 0x1FF;
    REG32(TICKS_BASE, TICKS_T1_CYC)  = cyc;
    REG32(TICKS_BASE, TICKS_T1_CTRL) = 1u;

    volatile uint32_t *vt = (volatile uint32_t *)(*(volatile uint32_t *)VTOR);
    vt[16 + TIMER1_IRQ] = (uint32_t)(uintptr_t)timer1_isr;
    *(volatile uint32_t *)NVIC_ICPR = (1u << TIMER1_IRQ);
    *(volatile uint32_t *)NVIC_ISER = (1u << TIMER1_IRQ);
    REG32(TIMER1_BASE, T_INTE) = 1u;
    REG32(TIMER1_BASE, T_ALARM0) = REG32(TIMER1_BASE, T_RAWL) + ISR_PERIOD_US;

    printf("[TBD16] Audio ready (SPI1 25MHz, DMA ch%d/%d, ISR %d Hz)\n",
           DMA_TX, DMA_RX, 1000000 / ISR_PERIOD_US);
}

void tbd16_audio_init_with_callback(uint32_t source_rate_hz,
                                     tbd16_audio_callback_t callback) {
    user_callback = callback;
    tbd16_audio_init(source_rate_hz);
}

void tbd16_audio_write(const int16_t *samples, uint32_t num_pairs) {
    uint32_t written = 0;
    while (written < num_pairs) {
        uint32_t free = ring_free();
        if (free == 0) {
            delay_us(100);
            continue;
        }
        uint32_t chunk = num_pairs - written;
        if (chunk > free) chunk = free;

        uint32_t w = ring_wr;
        for (uint32_t i = 0; i < chunk; i++) {
            uint32_t idx = (w + i) & RING_MASK;
            ring[idx * 2]     = samples[(written + i) * 2];
            ring[idx * 2 + 1] = samples[(written + i) * 2 + 1];
        }
        ring_wr = (w + chunk) & RING_MASK;
        written += chunk;
    }
}

uint32_t tbd16_audio_write_nb(const int16_t *samples, uint32_t num_pairs) {
    uint32_t free = ring_free();
    uint32_t count = num_pairs < free ? num_pairs : free;
    if (count == 0) return 0;

    uint32_t w = ring_wr;
    for (uint32_t i = 0; i < count; i++) {
        uint32_t idx = (w + i) & RING_MASK;
        ring[idx * 2]     = samples[i * 2];
        ring[idx * 2 + 1] = samples[i * 2 + 1];
    }
    ring_wr = (w + count) & RING_MASK;
    return count;
}

tbd16_audio_buffer_t *tbd16_take_buffer(void) {
    if (ring_free() < MIX_BUF_PAIRS) return 0;
    memset(mix_pcm, 0, sizeof(mix_pcm));
    mix_buf.sample_count = 0;
    return &mix_buf;
}

void tbd16_give_buffer(tbd16_audio_buffer_t *buf) {
    if (!buf || buf->sample_count == 0) return;
    int16_t *src = (int16_t *)buf->buffer->bytes;
    uint32_t n = buf->sample_count;
    if (n > RING_SIZE - 1) n = RING_SIZE - 1;

    uint32_t w = ring_wr;
    for (uint32_t i = 0; i < n; i++) {
        uint32_t idx = (w + i) & RING_MASK;
        ring[idx * 2]     = src[i * 2];
        ring[idx * 2 + 1] = src[i * 2 + 1];
    }
    ring_wr = (w + n) & RING_MASK;
}

uint32_t tbd16_audio_buffered(void) {
    return ring_count();
}

void tbd16_audio_test_tone(int enable) {
    test_tone_on = enable;
    tt_phase = 0;
    printf("[TBD16] Test tone %s\n", enable ? "ON (440 Hz)" : "OFF");
}
