# TBD-16 Audio Driver — Drop-In I2S Emulation for RP2040/RP2350 Projects

## The Problem

On most RP2040/RP2350 boards, audio goes directly from the Pico to a DAC via I2S (PIO), PWM, or similar. On the **DaDa Machines TBD-16**, the audio codec (TLV320AIC3254) is connected to the **ESP32-P4**, not the RP2350. The RP2350 has no direct path to the DAC.

```
  Standard Pico board:          TBD-16:
  ┌────────┐   I2S   ┌─────┐   ┌────────┐  SPI1   ┌────────┐  I2S   ┌─────┐
  │ RP2040 │────────▶│ DAC │   │ RP2350 │────────▶│ ESP32  │──────▶│ DAC │
  └────────┘  (PIO)  └─────┘   └────────┘  (DMA)  │  P4    │       └─────┘
                                                    └────────┘
```

This document explains exactly how audio gets from the RP2350 to the P4 codec, and provides a **drop-in driver** that any RP2040/RP2350 audio project can use to run on TBD-16 hardware.

---

## Architecture Overview

```
┌───────────────────────────────────────────────────────────────────────┐
│ RP2350B                                                               │
│                                                                       │
│  Your Audio Code                                                      │
│  ┌──────────────────────────────────────┐                             │
│  │ Generate samples (any rate)          │                             │
│  │ e.g. 44100, 48000, 49096, 49716 Hz  │                             │
│  └─────────────┬────────────────────────┘                             │
│                │ tbd16_audio_write(samples, count)                    │
│                ▼                                                      │
│  ┌──────────────────────────────────────┐                             │
│  │ Ring Buffer (2048 stereo pairs)      │                             │
│  │ SPSC lock-free, volatile indices     │                             │
│  └─────────────┬────────────────────────┘                             │
│                │ ISR reads ring buffer                                │
│                ▼                                                      │
│  ┌──────────────────────────────────────┐                             │
│  │ TIMER1 ISR @ 5000 Hz                │                             │
│  │ ├─ Read PWM edge counter (GPIO 27)  │ ◀── Codec word clock        │
│  │ ├─ Every 32 edges = 1 codec frame   │     (44100 Hz from P4)      │
│  │ ├─ Check P4 handshake (GPIO 22)     │                             │
│  │ ├─ Pack PCM2 into 512-byte frame    │                             │
│  │ └─ DMA transfer via SPI1            │                             │
│  └─────────────┬────────────────────────┘                             │
│                │ SPI1 DMA (25 MHz, Mode 3)                           │
│                │ GPIO 28-31                                           │
└────────────────┼──────────────────────────────────────────────────────┘
                 │
                 ▼
┌────────────────────────────────────────────────────────────────────────┐
│ ESP32-P4                                                               │
│  ┌──────────────────────────────────────┐                             │
│  │ SPI2 Slave (triple-buffered)         │                             │
│  │ Validates 0xCAFE + CRC               │                             │
│  └─────────────┬────────────────────────┘                             │
│                ▼                                                      │
│  ┌──────────────────────────────────────┐                             │
│  │ PicoAudioBridge Plugin               │                             │
│  │ ├─ Anti-aliasing biquad LPF (20kHz) │                             │
│  │ ├─ Cubic Hermite resampler           │ ← your_rate → 44100 Hz     │
│  │ ├─ Adaptive rate matching (±5%)      │                             │
│  │ └─ ×8 linear gain + hard clamp      │                             │
│  └─────────────┬────────────────────────┘                             │
│                ▼                                                      │
│  ┌──────────────────────────────────────┐                             │
│  │ TLV320AIC3254 Codec                  │                             │
│  │ I2S 44100 Hz, 32-bit stereo          │                             │
│  │ 32-sample DMA blocks                 │                             │
│  └──────────────────────────────────────┘                             │
└────────────────────────────────────────────────────────────────────────┘
```

---

## Hardware Wiring (Fixed on TBD-16 PCB)

### Link1 — Real-Time Audio (SPI1 ↔ P4 SPI2)

| Signal | RP2350 GPIO | P4 GPIO | Direction | Purpose |
|---|---|---|---|---|
| MISO | 28 | 29 | P4 → RP | Response data (ignored for audio) |
| CS | 29 | 28 | RP → P4 | Hardware-managed chip select |
| SCLK | 30 | 30 | RP → P4 | SPI clock (25 MHz) |
| MOSI | 31 | 31 | RP → P4 | Audio frame data |
| Handshake | 22 (in) | 51 (out) | P4 → RP | HIGH = P4 ready for next frame |
| Word Clock | 27 (in) | I2S WS | P4 → RP | 44100 Hz codec sample clock |

- **SPI Mode 3** (CPOL=1, CPHA=1), 25 MHz, 8-bit transfers
- **512-byte DMA frames** (must match P4 `STREAM_BUFFER_SIZE_`)
- **Handshake protocol**: P4 drives GPIO 51 HIGH when a slave SPI transaction is queued. RP2350 checks GPIO 22 before each DMA transfer.

### Link2 — Control (SPI0 ↔ P4 SPI3)

| Signal | RP2350 GPIO | P4 GPIO | Direction |
|---|---|---|---|
| MISO | 32 | 22 | P4 → RP |
| CS | 33 | 20 | RP → P4 |
| SCLK | 34 | 21 | RP → P4 |
| MOSI | 35 | 23 | RP → P4 |

- 2048-byte frames, byte-by-byte (no DMA)
- Used only for `SetActivePlugin("PicoAudioBridge")` at boot

---

## SPI Frame Protocol

Every audio frame is a 512-byte SPI transaction. The audio payload is embedded inside the TBD platform's standard SPI protocol.

### Frame Layout (512 bytes)

```
Offset  Size  Field
──────  ────  ─────────────────────────────────
0       2     Fingerprint: 0xCA 0xFE
2       16    Request Header
  2       2     magic: 0xCAFE
  4       1     sequence_counter (100-199, wrapping)
  5       5     reserved (zero)
  10      2     payload_length: 276
  12      2     payload_crc
  14      4     reserved (zero)
18      276   Request Payload (p4_spi_request2)
  18      4     magic: 0xFEEDC0DE
  22      4     synth_midi_length (PCM payload size)
  26      256   synth_midi[] — PCM2 audio data lives here
  282     4     sequencer_tempo (0 for audio-only)
  286     4     sequencer_active_track (0)
  290     4     magic2: 0xFEEDC0DE
294     218   Zero padding (fills to 512)
```

### PCM2 Payload (inside synth_midi[256])

```
Offset  Size  Field
──────  ────  ─────────────────────────────────
0       4     Magic: 0x50434D32 ("PCM2")
4       2     sample_count (uint16)
6       2     source_rate_hz (uint16, e.g. 49096, 44100)
8       N×4   Interleaved stereo int16: L0 R0 L1 R1 ...
              N = sample_count, max 62 pairs (248 bytes)
```

**Max payload**: 8 + 248 = 256 bytes (fills `synth_midi[]`).

### Samples Per Frame

The number of samples per frame is capped to match the P4 codec's consumption rate:

```
target_per_frame = source_rate × 32 / 44100
```

| Source Rate | Samples/Frame | Frame Rate |
|---|---|---|
| 44100 Hz | 32 | 1378 Hz |
| 48000 Hz | 34 | 1378 Hz |
| 49096 Hz | 35 | 1378 Hz |
| 49716 Hz | 36 | 1378 Hz |

**Critical**: Sending more samples than the P4 consumes per codec cycle overflows the P4 ring buffer, causing it to drop oldest samples (frequency distortion — e.g. 1000 Hz appears as ~559 Hz).

### CRC Algorithm

```c
uint16_t crc = 42;  // seed
for (int i = 0; i < payload_length; i++)
    crc += payload_bytes[i];
```

Simple additive checksum over the 276-byte `p4_spi_request2` struct.

### Sequence Counter

Wraps 100 → 199. Incremented per frame. The P4 uses this for frame deduplication.

---

## Word-Clock Synchronization

The RP2350 must deliver frames at exactly the rate the P4 codec consumes them. This is achieved by counting the codec's word clock (I2S WS) signal in hardware.

### How It Works

1. **GPIO 27** carries the P4 codec's I2S word-select signal at 44100 Hz
2. **PWM slice 5** is configured as a falling-edge counter on channel B (GPIO 27) — zero CPU overhead
3. **TIMER1 alarm ISR** fires at 5000 Hz, reads the PWM counter
4. Every **32 WS edges** = one codec DMA frame boundary
5. ISR sends exactly **one SPI frame per codec frame**
6. If the ISR falls behind (P4 handshake was low), it snaps to current frame count — no burst catching up

### Why This Matters

Without word-clock sync:
- **Free-running ISR** over-delivers frames → P4 ring overflow → timing compression (45s plays in 33.5s)
- **Polling from game loop** at ~35 fps → only 35 frames/sec vs 1378 needed → choppy audio

### PWM Edge Counter Setup

```c
// GPIO 27 → PWM slice 5, channel B
// CSR: DIVMODE=0b11 (falling edge on B), EN=1
REG32(PWM_BASE, PWM_CHn_CSR(5)) = (3u << 4) | 1u;
REG32(PWM_BASE, PWM_CHn_DIV(5)) = 1u << 4;      // DIV=1
REG32(PWM_BASE, PWM_CHn_CTR(5)) = 0;             // Reset
REG32(PWM_BASE, PWM_CHn_TOP(5)) = 0xFFFF;        // Free-running 16-bit
```

Counter wraps every ~1.486 sec (65536/44100). ISR reads delta every 200 µs (~8-9 edges), no risk of missed wraps.

---

## P4-Side Resampling

The P4's **PicoAudioBridge** plugin handles rate conversion from your source rate to the codec's 44100 Hz:

1. **Anti-aliasing LPF**: 2nd-order Butterworth biquad at 20 kHz (Q=0.7071), per-channel
2. **Cubic Hermite (Catmull-Rom) 4-tap resampler**: higher quality than linear interpolation
3. **Adaptive rate matching**: proportional control adjusts resample step based on ring buffer fill level (gain=0.005/256, clamped ±5%)
4. **Output gain**: ×8 linear with hard clamp at ±1.0

**Important for levels**: The P4 applies ×8 gain. Your int16 samples should peak around ±4096 (-18 dBFS) to avoid clipping. Full-scale int16 (±32767) will be hard-clipped after ×8.

---

## Boot Sequence

Before audio frames can flow, the RP2350 must tell the P4 which plugin to activate:

1. Wait for P4 codec word-clock pulses on GPIO 27 (confirms P4 is alive)
2. Send `SetActivePlugin(0, "PicoAudioBridge")` over Link2 (SPI0, GPIO 32-35)
3. Initialize Link1 SPI1 + DMA + TIMER1 ISR
4. Start generating audio

The control link uses SPI0, which on TBD-16 is shared with the SD card (GPIO 2-7). If your project uses the SD card, **initialize the control link after SD card operations are complete** (SPI0 gets reconfigured to GPIO 32-35).

---

## The RP2350 PSRAM / SDK Header Bug

On RP2350 with PSRAM (APS6404 on QMI CS1), including `stdbool.h` or Pico SDK headers like `hardware/timer.h` **before** `stdio.h` causes the PSRAM initialization to hang. This is why `p4_spi_transport.c` and `p4_control_link.c` use direct register access instead of SDK calls.

**Rule**: In any source file that runs before or during PSRAM init, `#include <stdio.h>` must be the **first include**. Do not include `hardware/*.h` headers in files that touch SPI1/DMA/timer registers.

If your project does not use PSRAM, you can safely use SDK headers everywhere.

---

## Drop-In Driver API

The driver provides a simple API that replaces `pico_audio_i2s` / PIO I2S / PWM audio. Your audio code generates samples, the driver handles everything else.

### Quick Start

```c
#include "tbd16_audio.h"

int main() {
    stdio_init_all();

    // Initialize audio transport (call once)
    tbd16_audio_init(44100);  // your source sample rate

    // Generate and send audio
    while (true) {
        int16_t samples[64];  // stereo interleaved: L0 R0 L1 R1 ...
        generate_your_audio(samples, 32);  // 32 stereo pairs

        tbd16_audio_write(samples, 32);  // blocks if ring buffer full
    }
}
```

### For pico-extras Projects (take/give buffer pattern)

Projects using the standard `take_audio_buffer` / `give_audio_buffer` pattern can use these compatibility functions:

```c
#include "tbd16_audio.h"

// Replace: struct audio_buffer_pool *ap = init_audio();
// With:
tbd16_audio_init(44100);

// Replace: struct audio_buffer *buffer = take_audio_buffer(ap, true);
// With:
audio_buffer_t *buffer = tbd16_take_buffer();

// Fill buffer->buffer->bytes with int16 stereo samples as usual
int16_t *samples = (int16_t *)buffer->buffer->bytes;
for (int i = 0; i < buffer->max_sample_count; i++) {
    samples[i * 2]     = left_sample;   // L
    samples[i * 2 + 1] = right_sample;  // R
}
buffer->sample_count = buffer->max_sample_count;

// Replace: give_audio_buffer(ap, buffer);
// With:
tbd16_give_buffer(buffer);
```

### For Callback-Based Projects (picoX7 style)

Projects that use a DMA callback to request samples can register a callback:

```c
#include "tbd16_audio.h"

// Your callback — called from ISR context at ~1378 Hz
void my_audio_callback(int16_t *stereo_buffer, uint32_t num_pairs) {
    for (uint32_t i = 0; i < num_pairs; i++) {
        int16_t sample = generate_next_sample();
        stereo_buffer[i * 2]     = sample;  // L
        stereo_buffer[i * 2 + 1] = sample;  // R
    }
}

int main() {
    tbd16_audio_init_with_callback(49096, my_audio_callback);
    // Audio runs autonomously via ISR — your main loop is free
    while (true) {
        // handle MIDI, UI, etc.
    }
}
```

---

## Complete Drop-In Header: tbd16_audio.h

```c
// tbd16_audio.h — Drop-in audio driver for DaDa Machines TBD-16
// Replaces pico_audio_i2s / PIO I2S / PWM audio with SPI transport to P4.
//
// Usage:
//   #include "tbd16_audio.h"
//   tbd16_audio_init(44100);
//   tbd16_audio_write(stereo_samples, num_pairs);
//
// The P4 codec runs at 44100 Hz. Your source rate can be anything
// (44100, 48000, 49096, 49716, etc.) — the P4 resamples automatically.

#ifndef TBD16_AUDIO_H
#define TBD16_AUDIO_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// ── Buffer type (compatible with pico-extras audio_buffer_t) ──────────
typedef struct tbd16_audio_buffer_mem {
    uint8_t *bytes;
    uint32_t size;
} tbd16_audio_buffer_mem_t;

typedef struct tbd16_audio_buffer {
    tbd16_audio_buffer_mem_t *buffer;
    uint32_t sample_count;
    uint32_t max_sample_count;
} tbd16_audio_buffer_t;

// Callback type for ISR-driven audio generation.
// Called at ~1378 Hz (once per codec frame).
// Fill stereo_buffer with num_pairs interleaved int16 L/R pairs.
typedef void (*tbd16_audio_callback_t)(int16_t *stereo_buffer, uint32_t num_pairs);

// ── Simple API ────────────────────────────────────────────────────────

// Initialize the TBD-16 audio transport.
// source_rate_hz: your audio sample rate (e.g. 44100, 48000, 49096).
// Handles: SPI1 init, DMA, word-clock sync, P4 plugin activation.
// Call once at startup, after stdio_init_all().
void tbd16_audio_init(uint32_t source_rate_hz);

// Write stereo audio samples to the P4 codec.
// samples: interleaved int16 stereo pairs (L0 R0 L1 R1 ...)
// num_pairs: number of stereo pairs (not individual samples).
// Blocks if the ring buffer is full (backpressure).
// Peak amplitude should be ~±4096 for clean output (P4 applies ×8 gain).
void tbd16_audio_write(const int16_t *samples, uint32_t num_pairs);

// Non-blocking version. Returns number of pairs actually written.
uint32_t tbd16_audio_write_nb(const int16_t *samples, uint32_t num_pairs);

// ── pico-extras Compatibility API ─────────────────────────────────────

// Get a buffer to fill (replaces take_audio_buffer).
// Returns NULL if ring buffer is full.
tbd16_audio_buffer_t *tbd16_take_buffer(void);

// Submit a filled buffer (replaces give_audio_buffer).
void tbd16_give_buffer(tbd16_audio_buffer_t *buf);

// ── Callback API ──────────────────────────────────────────────────────

// Initialize with a callback (replaces PIO I2S DMA IRQ pattern).
// The callback is called from the TIMER1 ISR at ~1378 Hz.
// Keep it fast — you have ~700 µs before the next call.
void tbd16_audio_init_with_callback(uint32_t source_rate_hz,
                                     tbd16_audio_callback_t callback);

// ── Utilities ─────────────────────────────────────────────────────────

// Number of stereo pairs currently buffered.
uint32_t tbd16_audio_buffered(void);

// Enable 440 Hz test tone (bypasses ring buffer, for debugging).
void tbd16_audio_test_tone(int enable);

#ifdef __cplusplus
}
#endif

#endif // TBD16_AUDIO_H
```

---

## Complete Drop-In Implementation: tbd16_audio.c

```c
// tbd16_audio.c — TBD-16 audio driver implementation
// See tbd16_audio.h for API documentation.
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

// DMA
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

#define RING_SIZE       2048
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

    // SPI0: 30 MHz, Mode 3, 8-bit
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

    // Wait for P4 ready
    deadline = time_us() + 100000;
    while (!gpio_in(18)) {  // CTL_RDY = GPIO 18
        if ((int32_t)(time_us() - deadline) >= 0) break;
    }

    // Send SetActivePlugin(0, "PicoAudioBridge")
    memset(ctl_tx, 0, sizeof(ctl_tx));
    ctl_tx[0] = 0xCA;
    ctl_tx[1] = 0xFE;
    ctl_tx[2] = 0x04;  // REQ_SET_ACTIVE_PLUGIN
    ctl_tx[3] = 0;     // channel 0
    const char *name = "PicoAudioBridge";
    int32_t nlen = strlen(name);
    memcpy(&ctl_tx[5], &nlen, 4);
    memcpy(&ctl_tx[9], name, nlen + 1);

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
            // Busy-wait briefly for ISR to drain
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
    if (n > MIX_BUF_PAIRS) n = MIX_BUF_PAIRS;

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
```

---

## Porting Guide: Step by Step

### 1. Find the Audio Output in Your Project

Look for one of these patterns:

| Pattern | Examples | What to Replace |
|---|---|---|
| **pico-extras I2S** | `audio_i2s_setup()`, `take_audio_buffer()` | Replace with `tbd16_audio_init()` + `tbd16_take_buffer()` |
| **pico-extras PWM** | `audio_pwm_setup()`, same buffer API | Same replacement as I2S |
| **Custom PIO I2S** | Direct PIO program, DMA IRQ callback | Use `tbd16_audio_init_with_callback()` |
| **PWM timer callback** | Timer ISR generates samples | Use `tbd16_audio_init_with_callback()` |
| **Blocking write loop** | Fill buffer, write to FIFO | Use `tbd16_audio_write()` |

### 2. Replace the Audio Init

**Before (pico-extras):**
```c
struct audio_format af = { .sample_freq = 44100, .format = AUDIO_BUFFER_FORMAT_PCM_S16, .channel_count = 2 };
struct audio_i2s_config cfg = { .data_pin = 28, .clock_pin_base = 26, .dma_channel = 0, .pio_sm = 0 };
audio_i2s_setup(&af, &cfg);
struct audio_buffer_pool *ap = audio_new_producer_pool(...);
audio_i2s_connect(ap);
audio_i2s_set_enabled(true);
```

**After (TBD-16):**
```c
tbd16_audio_init(44100);
```

### 3. Replace the Audio Loop

**Before:**
```c
struct audio_buffer *buf = take_audio_buffer(ap, true);
int16_t *samples = (int16_t *)buf->buffer->bytes;
// ... fill samples ...
buf->sample_count = buf->max_sample_count;
give_audio_buffer(ap, buf);
```

**After:**
```c
tbd16_audio_buffer_t *buf = tbd16_take_buffer();
if (buf) {
    int16_t *samples = (int16_t *)buf->buffer->bytes;
    // ... fill samples (same code) ...
    buf->sample_count = buf->max_sample_count;
    tbd16_give_buffer(buf);
}
```

### 4. Adjust Levels

The P4 applies **×8 gain**. If your project outputs near full-scale int16 (±32767), the output will clip.

| Your Peak | After ×8 | Result |
|---|---|---|
| ±4096 (-18 dBFS) | ±32768 | Full scale, clean |
| ±2048 (-24 dBFS) | ±16384 | Half scale, plenty of headroom |
| ±16384 (-6 dBFS) | ±131072 | **Hard clipped** at ±32767 |
| ±32767 (0 dBFS) | ±262136 | **Severely clipped** |

**Fix**: Attenuate your output by right-shifting: `sample >>= 3` (÷8).

### 5. Remove pico-extras Dependency

Remove from your CMakeLists.txt:
```cmake
# Remove these:
target_link_libraries(... pico_audio_i2s)
# And the PICO_EXTRAS_PATH stuff
```

### 6. If Your Project Uses Mono Output

The driver expects stereo interleaved (L R L R). For mono:
```c
int16_t mono_sample = generate_sample();
int16_t stereo[2] = { mono_sample, mono_sample };
tbd16_audio_write(stereo, 1);
```

---

## Example: Porting picoX7 to TBD-16

picoX7 uses a callback pattern via `MTL::Audio::getSamples()`:

```cpp
// ORIGINAL (picoX7)
static HWR::Audio<SAMPLES_PER_TICK> audio{DAC_FREQ, true};
MTL_AUDIO_ATTACH_IRQ_0(audio);

void MTL::Audio::getSamples(uint32_t* buffer, unsigned n) {
    for (unsigned i = 0; i < SAMPLES_PER_TICK; i += 2)
        buffer[i + 1] = dx7.getSamplePair(0, NUM_VOICES / 2);
    dx7.tick(0, NUM_VOICES / 2);
}
audio.start();
```

```c
// TBD-16 PORT
#include "tbd16_audio.h"

void tbd16_dx7_callback(int16_t *stereo_buf, uint32_t num_pairs) {
    for (uint32_t i = 0; i < num_pairs; i++) {
        // DX7 returns packed L|R in uint32_t
        uint32_t pair = dx7.getSamplePair(0, NUM_VOICES);
        stereo_buf[i * 2]     = (int16_t)(pair & 0xFFFF);         // L
        stereo_buf[i * 2 + 1] = (int16_t)((pair >> 16) & 0xFFFF); // R
    }
    dx7.tick(0, NUM_VOICES);
}

int main() {
    stdio_init_all();
    tbd16_audio_init_with_callback(49096, tbd16_dx7_callback);
    // ... rest of main loop (MIDI, UI, etc.)
}
```

---

## Gotchas and Lessons Learned

### 1. The Samples-Per-Frame Bug

**Symptom**: All frequencies wrong (e.g. 1000 Hz plays as 559 Hz).

**Cause**: Sending more samples per SPI frame than the P4 consumes per codec cycle. The P4 ring buffer overflows and drops oldest samples, changing the effective sample rate.

**Fix**: Cap to `source_rate × 32 / 44100` samples per frame. The driver does this automatically.

### 2. The Timing Compression Bug

**Symptom**: Correct pitch but playback too fast (45s of audio in 33.5s).

**Cause**: Free-running ISR catches P4 handshake HIGH multiple times per codec cycle, sending too many frames per second.

**Fix**: Word-clock synchronization via PWM edge counter. Only send when 32 new WS edges have accumulated.

### 3. The PSRAM Init Hang

**Symptom**: Board hangs at boot, no serial output.

**Cause**: Including `stdbool.h` or SDK headers before `stdio.h` corrupts the PSRAM initialization sequence on RP2350.

**Fix**: Always `#include <stdio.h>` first. Use direct register access instead of SDK calls in audio transport code. If your project doesn't use PSRAM, you can ignore this.

### 4. TIMER1 Tick Generator

**Symptom**: TIMER1 alarm never fires (TIMERAWL reads 0 forever).

**Cause**: The Pico SDK only enables TIMER0's tick generator. TIMER1 has its own tick in the TICKS peripheral.

**Fix**:
```c
uint32_t cyc = REG32(TICKS_BASE, 0x1c) & 0x1FF;  // Copy from TIMER0
REG32(TICKS_BASE, 0x28) = cyc;   // TIMER1_CYCLES
REG32(TICKS_BASE, 0x24) = 1u;    // TIMER1_CTRL = ENABLE
```

### 5. DMA CTRL Register Bit Positions Differ on RP2350

- **INCR_WRITE** is bit 6 (not bit 5 like RP2040)
- **TREQ_SEL** is bits [22:17] (not [20:15])
- **CHAIN_TO** is bits [16:13] (not [14:11])
- **BUSY** is bit 26 (not bit 24)
- **MULTI_CHAN_TRIGGER** is at offset 0x450 (not 0x430)

The driver handles this correctly. If you're writing your own DMA code, use the RP2350 datasheet, not RP2040 examples.

### 6. SPI0 Shared Between SD Card and Control Link

SPI0 is used for both the SD card (GPIO 2-7) and the P4 control link (GPIO 32-35). Initialize the control link **after** SD card operations are complete. The SD card library may put SPI0 into reset — unreset it before using for control.

### 7. P4 RDY Window

The P4 audio task processes SPI receive and codec write sequentially. RDY is HIGH for only ~725 µs per cycle (during `Codec::WriteBuffer`). The ISR must be fast enough to catch this window — that's why it runs at 5000 Hz (200 µs period).

---

## Performance

| Metric | Value |
|---|---|
| ISR fire rate | 5000 Hz |
| ISR CPU overhead | ~6% |
| Frame delivery rate | 1378 Hz (locked to codec) |
| SPI DMA transfer time | ~164 µs per 512-byte frame |
| Ring buffer latency | ~1-2 ms (2048 pairs at 44100 Hz ≈ 46 ms max) |
| P4 resampler latency | ~5.8 ms (256-pair watermark) |
| Total end-to-end latency | ~8-10 ms |

---

## Files Reference (from dada-tbd-doom)

These are the source files in the Doom project that implement the audio bridge. The drop-in driver above is a self-contained extraction of this code.

| File | Purpose |
|---|---|
| `src/pico_audio_bridge.h` | Ring buffer + PCM packing API |
| `src/pico_audio_bridge.c` | Ring buffer, PCM2 framing, test tone |
| `src/p4_spi_transport.h` | SPI1 DMA transport API |
| `src/p4_spi_transport.c` | SPI1 init, DMA, TIMER1 ISR, word-clock sync |
| `src/p4_control_link.h` | Control link (SetActivePlugin) API |
| `src/p4_control_link.c` | SPI0 control link, P4 alive detection |
| `src/SpiProtocol.h` | SPI frame structures (shared with P4) |
| `src/pico/audio_i2s.h` | Stub types (replaces pico-extras header) |
| `src/i_picosound.c` | Doom sound module (example consumer) |
