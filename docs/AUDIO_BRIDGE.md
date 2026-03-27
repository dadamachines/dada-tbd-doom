# TBD-16 PicoAudioBridge — Complete System Reference

## Overview

The TBD-16 runs Doom on the RP2350B co-processor, but audio output is handled by
the ESP32-P4 main processor through its TLV320AIC3254 codec (I2S, 44100 Hz,
32-bit stereo, 32-sample blocks). The RP2350 generates Doom's OPL music and SFX
at 49716 Hz and streams raw PCM2 frames (up to 62 stereo pairs per SPI frame)
over SPI1 DMA to the P4 at ~1400 frames/sec. The P4's PicoAudioBridge plugin
resamples from 49716→44100 Hz using a cubic Hermite (Catmull-Rom) 4-tap
resampler with adaptive rate matching.

This document covers the full system architecture and current status.

## System Architecture

```
 RP2350B (Doom Engine, Core 0)          ESP32-P4 (Audio Engine)
┌─────────────────────────────┐        ┌──────────────────────────────┐
│                             │        │                              │
│ EMU8950 OPL Music (49716Hz) │        │  SPManager::audio_task()     │
│         │                   │        │     │                        │
│         ▼                   │        │     ├─ PrepareResponse       │
│ SFX Mixer + Fade (49716Hz)  │        │     ├─ QueueBuffer ──────┐   │
│         │                   │        │     │          (blocks)  │   │
│         ▼                   │        │     ├─ GetReceivedBuffer │   │
│  Ring Buffer (SPSC)         │        │     │     (blocks ~364µs)│   │
│  (2048 stereo pairs)        │        │     ├─ Validate + Process│   │
│         │                   │        │     │                    │   │
│  ISR reads ──────►          │        │     ├─ Codec::ReadBuffer │   │
│         │                   │        │     ├─ sp[0]->Process()  │   │
│  No resampling              │        │     ├─ Codec::WriteBuffer│   │
│  (raw 49716 Hz to P4)       │        │     │     (blocks ~725µs)│   │
│         │                   │        │     └────────────────────┘   │
│  TIMER1 Alarm ISR (5000 Hz) │        │                              │
│    pab_pack_spi() ─────┐    │        │  SPI2 Slave (triple-buffered)│
│    pack_frame()        │    │  SPI1  │    QueueBuffer → handshake   │
│    start_dma() ────────┼────┼──DMA──►│    GetReceivedBuffer         │
│         │              │    │ 25MHz  │         │                    │
│    (non-blocking)      │    │ 512B   │         ▼                    │
│                        │    │        │  PicoAudioBridge::Process()  │
│  Game loop (~35 fps)   │    │        │    AA LPF → resample → ×8    │
│    I_Pico_UpdateSound()│    │        │         │                    │
│      fills ring buffer │    │        │         ▼                    │
│                        │    │        │  TLV320AIC3254 Codec (I2S)   │
│                        │    │        │    44100 Hz, 32-bit stereo   │
└─────────────────────────────┘        └──────────────────────────────┘
         │
         │ Control SPI (spi0, 2048B)
         └──────────────────────────► SetActivePlugin("PicoAudioBridge")
```

## Hardware Links

### Link1 — Real-time Audio (SPI1 ↔ SPI2_HOST)

| Signal    | RP2350 GPIO | P4 GPIO | Direction |
|-----------|-------------|---------|-----------|
| MISO (RX) | 28          | 29      | P4 → RP   |
| CS        | 29          | 28      | RP → P4   |
| SCLK      | 30          | 30      | RP → P4   |
| MOSI (TX) | 31          | 31      | RP → P4   |
| Handshake | 22 (input)  | 51 (output) | P4 → RP |

- **SPI Mode 3** (CPOL=1, CPHA=1), 25 MHz clock, 8-bit transfers
- **512-byte DMA frames** (STREAM_BUFFER_SIZE_ on both sides)
- **Handshake**: P4 sets GPIO 51 HIGH when a slave transaction is queued
  (ready to receive). RP2350 ISR checks GPIO 22 before each DMA transfer.
- **Triple-buffered** on P4 side (3 DMA transaction slots)

### Link2 — Control (SPI0 ↔ SPI3_HOST)

| Signal    | RP2350 GPIO | P4 GPIO | Direction |
|-----------|-------------|---------|-----------|
| MISO      | 32          | 22      | P4 → RP   |
| CS        | 33          | 20      | RP → P4   |
| SCLK      | 34          | 21      | RP → P4   |
| MOSI      | 35          | 23      | RP → P4   |

- 2048-byte frames, byte-by-byte (no DMA), used for `SetActivePlugin` etc.

## Data Flow

### 1. Game Loop — Audio Generation (i_picosound.c)

`I_Pico_UpdateSound()` runs at ~35 fps in the Doom game loop and fills the ring
buffer as fast as possible:

- Calls OPL music generator (EMU8950) → int16 stereo at 49716 Hz
- **OPL ×8 gain** (`<<= 3` in `opl_pico.c`): Applied in-place on int16,
  matching the original rp2040-doom exactly. Wraps on overflow (intentional —
  the OPL output is quiet enough that wrapping rarely occurs at default music
  volume).
- Mixes ADPCM SFX channels (int8 decoded, pitch-shifted, stereo-panned)
  additively into the same int16 buffer. SFX volume scaling:
  `sample * (vol / 2)` where sample is int8 (±127) and vol is 0–255.
  No clamping — additive mix wraps on int16 overflow (same as original).
- **Multicore lock**: `__sync_lock_test_and_set` / `__sync_lock_release`
  (LDREX/STREX atomics) prevents Core 0 and Core 1 from calling the mixer
  simultaneously. Non-blocking try-lock — if held, the caller skips.
- Applies fade in/out
- Pushes ~1400 stereo samples per frame into ring buffer via `pab_give_buffer()`
- Calls `p4_spi_transport_poll()` (now only prints debug stats)

### 2. Timer ISR — SPI Transport (p4_spi_transport.c)

A TIMER1 alarm ISR fires at **5000 Hz** (200 µs period) and handles all SPI
frame transmission independent of the game loop:

1. Clears alarm interrupt, re-arms at `now + 200 µs`
2. Checks if previous DMA is still busy → skip if yes
3. Checks P4 handshake GPIO 22 (HIGH = ready) → skip if LOW
4. `pack_frame()` → `pab_pack_spi()` reads up to 62 raw stereo int16 samples
   from the ring buffer (no resampling), packs into a PCM2 frame in
   `p4_spi_request2.synth_midi[]`. Returns 0 if ring buffer is empty (skips
   frame entirely to avoid injecting silence into P4 ring buffer).
5. `start_dma()` → configures TX+RX DMA channels, triggers both simultaneously
6. Returns immediately (DMA completes in background, ~164 µs)

The ISR is **non-blocking** — it never waits. DMA finishes in the background
and the next ISR invocation checks completion before sending again.

### 3. P4 Audio Task — Receive & Codec Output (SPManager.cpp)

`SPManager::audio_task()` runs as a FreeRTOS task in a tight loop:

1. **PrepareResponse**: Packs USB MIDI, waveforms, Ableton Link data
2. **QueueBuffer**: Queues prepared response into SPI slave (sets handshake HIGH)
3. **Codec::WriteBuffer**: Writes 32 samples to I2S output (~725 µs blocking),
   during which the SPI transfer from step 2 completes in parallel
4. **GetReceivedBuffer**: SPI transaction should be complete by now (~364 µs)
5. **Validate**: Checks 0xCAFE watermark, CRC, sequence counter
6. **Process**: Copies `synth_midi[]` → `pd.midi_bytes[]`, calls sound processors
7. **Codec::ReadBuffer**: Reads 32 samples from I2S input
8. **Sound processors**: `sp[0]->Process(pd)` — PicoAudioBridge pushes PCM2
   samples into its ring buffer, resamples 49716→44100 Hz via cubic Hermite,
   outputs 32 float samples

## SPI Protocol

### Frame Layout (512 bytes, master → slave)

```
Offset  Size  Field
0       2     Watermark: 0xCA 0xFE (validated by rp2350_spi_stream)
2       16    p4_spi_request_header
  2       2     magic: 0xCAFE
  4       1     request_sequence_counter (100-199, wrapping)
  5       5     reserved
  10      2     payload_length (sizeof p4_spi_request2 = 276)
  12      2     payload_crc
  14      4     reserved
18      276   p4_spi_request2
  18      4     magic: 0xFEEDC0DE
  22      4     synth_midi_length (136 for PCM audio)
  26      256   synth_midi[] (PCM payload)
  282     4     sequencer_tempo (bpm × 100)
  286     4     sequencer_active_track
  290     4     magic2: 0xFEEDC0DE
294     218   Unused (zero-padded to 512)
```

### CRC Algorithm

```c
uint16_t crc = 42;
for (i = 0; i < payload_length; i++) crc += payload[i];
```

Simple additive checksum with seed 42, computed over the `p4_spi_request2` payload.

### PCM Payload in `synth_midi[256]`

**PCM2 protocol** (current, used by dada-tbd-doom):

```
Offset  Size  Field
0       4     PCM2 magic: 0x50434D32 ("PCM2")
4       2     sample_count (uint16, 1-62 stereo pairs)
6       2     source_rate_hz (uint16, e.g. 49716)
8       N×4   N interleaved stereo int16 samples (L0 R0 L1 R1 ...)
              N = sample_count, max 62 × 4 = 248 bytes
```

Max payload: 8 + 248 = 256 bytes (fills entire `synth_midi[]`).
At full rate with 62 stereo pairs per frame at ~800 Hz effective: ~200 KB/s.

**Legacy PCM! protocol** (backward compat, no longer sent):

```
Offset  Size  Field
0       4     PCM magic: 0x50434D21 ("PCM!")
4       4     Sample count: 32
8       128   32 interleaved stereo int16 samples
```

### SpiProtocolHelper State Machine (P4 side)

The `SpiProtocolHelper` manages a simple state machine for request/response
sequencing:

```
canPrepareNextResponse = true   ← initial state
         │
shouldPrepareNextResponse()     ← returns true
         │
markNextResponsePrepared()      ← canPrepare=false, nextPrepared=true
         │
shouldSendPreparedResponse()    ← returns true
         │
queuedPreparedResponse()        ← nextPrepared=false, advances seq counter
         │
[wait for SPI transaction]
         │
markRequestSeen(seq)            ← canPrepare=true (cycle restarts)
```

Sequence counters wrap 100→199 for both request and response.

## Resampling

Doom with `USE_EMU8950_OPL=1` generates audio at **49716 Hz** (native OPL clock
÷ 72). The TBD codec runs at **44100 Hz**.

### RP2350 Side — No Resampling

The RP2350 sends raw 49716 Hz samples via PCM2 frames. No resampling on the
RP2350 side. The ring buffer stores 49716 Hz stereo pairs (2048 pairs, power
of 2 for fast masking, SPSC lock-free). `pab_pack_spi()` simply copies up to
62 stereo pairs per frame from the ring buffer.

When the ring buffer is empty, `pab_pack_spi()` returns 0 (no frame sent),
avoiding the injection of zero-value samples that would disrupt the P4's
resampler state.

### P4 Side — Cubic Hermite Resampling

The PicoAudioBridge plugin on P4 performs 49716→44100 Hz resampling using:

- **Ring buffer**: 4096 stereo pairs with watermark pre-fill (256 pairs)
- **Anti-aliasing filter**: 2nd-order Butterworth biquad LPF at 20 kHz
  (Q=0.7071, bilinear transform), applied per-channel in `ring_push()` before
  storage. Prevents aliasing when decimating from 49716→44100 Hz.
- **Cubic Hermite (Catmull-Rom) 4-tap resampler**: Higher quality than linear
  interpolation, uses 4 surrounding samples for smooth interpolation.
  All arithmetic is **float** (no fixed-point).
- **Adaptive rate matching**: Proportional control adjusts resample step based
  on ring buffer fill level vs target (RING_WATERMARK = 256):
  `adjustment = fill_error * (0.005 / WATERMARK)`, clamped to ±5% of nominal.
  Float precision eliminates the integer dead zone of fixed-point.
- **Output gain**: ×8 linear gain with hard clamp at ±1.0. Brings the quiet
  Doom mixer output (~12% of full scale) to proper DAC level without
  waveshaping. Combined with OPL's `<<= 3`, total gain is ×64.
- **Graceful underflow**: On ring buffer underrun, outputs available samples
  without resetting resample fractional state, preserving phase continuity
- **Overflow protection**: On ring_push overflow, drops oldest samples

## P4 Plugin: PicoAudioBridge

- Stereo plugin (output only, no input processing)
- No CV / TRIG (dummy entries only)
- `Process()`: Parses PCM2 header from `data.midi_bytes`, pushes stereo int16
  samples through anti-aliasing biquad LPF into internal ring buffer (4096
  pairs), resamples 49716→44100 Hz via float cubic Hermite interpolation,
  applies ×8 linear gain with hard clamp, writes `data.buf`
- **Ring buffer watermark**: First 256 pairs must accumulate before output begins
  (`pcm2_primed` flag). This absorbs SPI jitter and provides resampler lookahead.
- **Adaptive rate matching**: Adjusts resample step proportionally based on
  ring fill vs watermark target. Gain = 0.005/WATERMARK, clamped ±5%.
- **`pcm2_active` flag**: Once set, the plugin always drains its ring buffer
  (even if current frame has no PCM2 data), preventing brief silence gaps.
- If no PCM magic detected and not active, outputs silence (graceful fallback)
- Registered via `SetActivePlugin(0, "PicoAudioBridge")` from RP2350 at boot
- Block size: 32 stereo samples (BUF_SZ)

## Test Tone

A 440 Hz sine test tone can be enabled for debugging:
```c
pab_set_test_tone(true);   // in src/i_picosound.c
```
When enabled, `pab_pack_spi()` generates a pure sine wave directly,
bypassing the ring buffer and resampler. Use this to isolate whether
distortion is in the SPI/P4 chain or in the audio pipeline.

The test tone bypasses the OPL and SFX mixer entirely, useful for isolating
whether issues are in the SPI/P4 chain or in the audio pipeline.

## RP2350 Timer ISR Transport

### Why a Timer ISR (not poll-based)

The original implementation polled for P4 readiness and sent frames during
`p4_spi_transport_poll()` called from the game loop at ~35 fps. This couldn't
deliver the 1378 frames/sec needed by the P4 codec (44100 Hz ÷ 32 samples).

The current implementation uses **TIMER1 ALARM0** to fire an ISR at 5000 Hz,
completely decoupled from the game loop timing.

### TIMER1 Register Map

| Register | Address | Offset | Purpose |
|----------|---------|--------|---------|
| TIMERAWL | TIMER1+0x28 | 0x28 | Current timer value (µs) |
| ALARM0   | TIMER1+0x10 | 0x10 | Alarm compare value |
| ARMED    | TIMER1+0x20 | 0x20 | Armed status |
| INTR     | TIMER1+0x3c | 0x3c | Interrupt raw (write 1 to clear) |
| INTE     | TIMER1+0x40 | 0x40 | Interrupt enable |

- **TIMER1 base**: `0x400b8000` (distinct from TIMER0 at `0x400b0000`)
- **Reset bit**: bit 24 in RESETS register
- **IRQ**: TIMER1_IRQ_0 = IRQ 4 on RP2350
- **NVIC**: ISER at `0xE000E100`, ICPR at `0xE000E280`

### Tick Generator — Critical Detail

The RP2350 SDK only enables TIMER0's tick generator. TIMER1 has its own tick
generator in the TICKS peripheral that must be enabled separately or TIMER1
just reads zero and never fires alarms:

```c
// TICKS_BASE = 0x40108000
// Copy TIMER0's cycles value (12 for 12 MHz XOSC → 1 µs ticks)
uint32_t tick_cycles = REG32(TICKS_BASE, 0x1c) & 0x1FF;
REG32(TICKS_BASE, 0x28) = tick_cycles;  // TIMER1_CYCLES
REG32(TICKS_BASE, 0x24) = 1u;           // TIMER1_CTRL = ENABLE
```

### ISR Installation

The ISR is patched into the RAM vector table (VTOR points to RAM on RP2350):

```c
volatile uint32_t *vtable = (volatile uint32_t *)(*(volatile uint32_t *)0xE000ED08);
vtable[16 + 4] = (uint32_t)(uintptr_t)timer1_alarm0_isr;
```

### ISR Flow

```
timer1_alarm0_isr() fires every 200 µs:
  ├── Clear INTR bit 0
  ├── Re-arm: ALARM0 = now + 200
  ├── dbg_isr_fires++
  ├── DMA busy? → dbg_isr_dma_busy++, return
  ├── RDY LOW?  → dbg_isr_rdy_low++, return
  └── pack_frame() + start_dma() → dbg_frames_sent++
```

### Performance (measured)

| Metric | Value | Target |
|--------|-------|--------|
| ISR fire rate | 4755 Hz | 5000 Hz |
| DMA busy skips | 13.6% | Expected (164µs DMA / 200µs period) |
| **RDY LOW skips** | **72.1%** | **Should be <30%** |
| Frame delivery rate | **678 Hz** | **1378 Hz needed** |
| ISR overhead | ~6% CPU | Acceptable |

### Why No SDK Headers

Including `stdbool.h` or Pico SDK headers before `stdio.h` causes the RP2350B
PSRAM initialization to hang. All register access in `p4_spi_transport.c` and
`p4_control_link.c` uses direct `#define` constants — no `hardware/timer.h`,
no `hardware/dma.h`, no `hardware/spi.h`.

---

## Current Status

| Component | Status | Notes |
|-----------|--------|-------|
| PSRAM warm-boot | ✅ Working | Exit QPI + Reset + 200µs delay, 3 retries (cold boot needed after debug probe reset) |
| RP2350 ring buffer (SPSC) | ✅ Working | 2048 stereo pairs, lock-free, volatile indices |
| PCM2 raw transport | ✅ Working | Up to 62 stereo pairs per frame, no RP2350-side resampling |
| SPI1 DMA transport | ✅ Working | 25 MHz Mode 3, channels 4/5, 512-byte frames |
| TIMER1 alarm ISR | ✅ Working | 5000 Hz, non-blocking, ~6% CPU |
| P4 pipeline reorder | ✅ Working | WriteBuffer before GetReceivedBuffer, ~1400 Hz throughput |
| P4 cubic Hermite resampler | ✅ Working | 49716→44100 Hz, 4-tap Catmull-Rom |
| P4 anti-aliasing filter | ✅ Working | 2nd-order Butterworth biquad LPF at 20 kHz |
| P4 ring buffer (4096) | ✅ Working | Watermark 256, overflow protection, graceful underflow |
| P4 adaptive rate matching | ✅ Working | Float precision, gain=0.005/WATERMARK, ±5% clamp |
| P4 output gain (×8 linear) | ✅ Working | Hard clamp ±1.0, total chain gain ×64 |
| Int16 mixer | ✅ Working | Same as original: `<<= 3` in OPL + additive SFX, wraps |
| Multicore atomic lock | ✅ Working | LDREX/STREX try-lock, non-blocking, both cores safe |
| SFX pitch correction | ✅ Working | Divides by NORM_PITCH (127), not by pitch (was no-op) |
| Test tone generator | ✅ Working | 440 Hz sine, bypasses ring buffer |
| P4 SPI slave receive | ✅ Working | Triple-buffered, handshake, CRC validation |
| Control link (SPI0) | ✅ Working | SetActivePlugin at boot |
| Skip empty frames | ✅ Working | `pab_pack_spi` returns 0 when ring empty |
| SOUND_LOW_PASS filter | ❌ Disabled | Was too aggressive (558 Hz cutoff); Graham: "didn't produce any noticeable improvement" |

**Sound quality**: Close to original. Linear gain chain matches rp2040-doom
character. Remaining difference: 49716→44100 Hz resampling (unavoidable with
TLV320 codec).

---

## P4-Side Bottleneck — Detailed Analysis

### The Problem

The P4's `audio_task()` processes SPI receive and codec write **sequentially**:

```
┌─ QueueBuffer ──────┐  Queues SPI slave transaction, sets RDY HIGH
│                    │  
├─ GetReceivedBuffer ┤  Blocks ~364 µs waiting for SPI slave to complete
│                    │  (RDY goes LOW during this time)
├─ Process + Validate┤  ~10 µs
│                    │
├─ Codec::ReadBuffer ┤  ~10 µs
├─ sp[0]->Process()  ┤  ~10 µs
├─ Codec::WriteBuffer┤  Blocks ~725 µs writing to I2S DMA
│                    │
└────────────────────┘
Total ≈ 1099 µs per cycle → max 910 Hz
```

The RP2350 sees RDY HIGH for only ~200 µs out of every 1099 µs cycle (the brief
window between QueueBuffer and GetReceivedBuffer completing). RDY is LOW for the
remaining ~900 µs, causing the ISR to skip 72% of its attempts.

### Why 678 Hz, Not 910 Hz

At 910 Hz theoretical max, there's additional overhead from:
- `PrepareResponse` logic (pack waveforms, MIDI, Link data)
- `taskYIELD()` calls (relinquishes CPU briefly)
- FreeRTOS scheduler jitter
- Occasional `printf` calls

These push the effective rate down to ~678 Hz.

### The Math

The codec operates at 44100 Hz with 32-sample blocks:
- **Required frame rate**: 44100 / 32 = **1378.125 Hz**
- **Current delivery rate**: ~678 Hz
- **Deficit**: 700 frames/sec = 700 × 32 = 22,400 samples/sec of silence
- **Result**: ~50% of codec blocks play silence → distorted, choppy audio

### ISR Diagnostic Evidence

Debug output from RP2350 (5-second capture):
```
[P4-SPI] sent=3390 isr=23775 dma_busy=3230 rdy_low=17155 rdy=0
```
- ISR fires: 23775 / 5s = **4755 Hz** ✓ (close to 5000 target)
- DMA busy: 3230 / 23775 = **13.6%** (expected: 164µs DMA / 200µs ≈ 82% overlap)
- **RDY LOW: 17155 / 23775 = 72.1%** ← P4 not ready most of the time
- Actually sent: 3390 / 5s = **678 Hz** ← half of what's needed

---

## Compatibility with Other Plugins & RP2350 Firmware

The TBD-16 platform uses the same SPI transport and `audio_task` loop for all
RP2350↔P4 communication. The **tbd-pico-seq3** firmware drives the
**PicoSeqRack** plugin through the exact same `p4_spi_request2` frames over the
same SPI1 bus. Any changes to the P4 firmware **must not break** seq3 or any
other existing plugin.

### How the Transport Is Shared

```
 tbd-pico-seq3                    dada-tbd-doom (Doom)
┌──────────────┐                 ┌──────────────────────┐
│ Sequencer    │                 │ OPL + SFX + resample │
│ MIDI bytes → │                 │ PCM 0x50434D21 →     │
│ synth_midi[] │                 │ synth_midi[]         │
└──────┬───────┘                 └──────────┬───────────┘
       │  spi1, 512B, Mode3, GPIO 28-31    │
       └───────────────┬───────────────────┘
                       │  (only one RP2350 firmware at a time)
                       ▼
              ┌────────────────┐
              │ P4 audio_task  │  ← SPManager.cpp
              │                │
              │ synth_midi[] → │
              │ pd.midi_bytes  │
              │       │        │
              │       ▼        │
              │  sp[0]->Process│  ← whichever plugin is active
              └────────────────┘
                    │
        ┌───────────┼───────────┐
        ▼           ▼           ▼
  PicoSeqRack  PicoAudioBridge  (any other plugin)
  interprets   interprets       interprets
  raw MIDI     PCM magic        midi_bytes its own way
```

**Key architectural fact**: SPManager is a **dumb pipe**. It copies
`synth_midi[]` → `pd.midi_bytes[]` without parsing. Each plugin's `Process()`
method interprets the data in its own way. The SPI transport, handshake,
protocol helper, and codec I/O are 100% plugin-agnostic.

### seq3 vs Doom: What Differs, What's Shared

| Aspect | tbd-pico-seq3 | dada-tbd-doom | Shared? |
|--------|--------------|---------------|---------|
| RP2350 SPI bus | spi1, GPIO 28-31, 30 MHz | spi1, GPIO 28-31, 25 MHz | Same bus |
| Handshake GPIO | 22 (input, pulldown) | 22 (input, pulldown) | Same pin |
| Frame format | `p4_spi_request2` (276 bytes) | Same struct | **Identical** |
| `synth_midi[]` content | Raw MIDI bytes (note on/off, CC) | PCM: `0x50434D21` + int16 samples | **Different** |
| `synth_midi_length` | Byte count of MIDI data (varies) | 136 (fixed: 8 hdr + 128 PCM) | Different |
| `sequencer_tempo` | BPM × 100 (e.g. 12000) | 0 (unused) | — |
| Timing | Handshake-gated poll (~1 ms loop) | TIMER1 ISR at 5000 Hz | Different |
| Frame rate need | ~1378 Hz (codec-locked) | ~1378 Hz (codec-locked) | **Same** |
| Active plugin | PicoSeqRack | PicoAudioBridge | Different |
| Reads P4 response? | Yes (USB MIDI, waveforms, Link) | No (ignores rx_buf) | Different |
| Control SPI | spi0, GPIO 32-35 | spi0, GPIO 32-35 | Same bus |
| `SpiProtocol.h` | Original | Copy (identical structs) | **Identical** |
| `SpiProtocolHelper` | In RP2350 (MidiP4_2.cpp) | In P4 (SPManager.cpp) | Same logic |

### Impact Analysis of Each Proposed P4 Change

#### 1. Pipeline Fix (reorder audio_task loop) — SAFE FOR ALL PLUGINS

**Do we need a mode switch?** No. The pipeline reorder is universally safe and
even slightly beneficial for other plugins. Here's the detailed reasoning:

##### What the reorder actually does

**Current loop order** (all plugins see this):
```
PrepareResponse → QueueBuffer → GetReceivedBuffer(364µs) → [extract midi] →
taskYIELD → Codec::ReadBuffer → sp[0]->Process → Codec::WriteBuffer(725µs)

RDY is HIGH for ~200µs (between QueueBuffer and GetReceivedBuffer completing)
```

**Proposed loop order**:
```
PrepareResponse → QueueBuffer → Codec::WriteBuffer(725µs) →
GetReceivedBuffer → [extract midi] → taskYIELD →
Codec::ReadBuffer → sp[0]->Process

RDY is HIGH for ~725µs (entire duration of Codec::WriteBuffer)
```

##### Why PicoSeqRack is UNAFFECTED

1. **Data flow is identical**: MIDI bytes still arrive via `GetReceivedBuffer`,
   still get copied to `pd.midi_bytes`, still get passed to `sp[0]->Process()`.
   PicoSeqRack still parses note on/off, CC, pitch bend exactly the same way.
   The synthesizer still produces audio into `fbuf`. Nothing changes about
   *what* data goes where.

2. **+0.726 ms audio latency is inaudible**: The output from `Process()` at
   iteration N is written to the codec at iteration N+1 instead of N. This
   adds one codec block (32/44100 = 0.726 ms). Professional MIDI gear routinely
   has 3-5 ms of latency. The human perception threshold is ~5-10 ms. This
   increase is undetectable.

3. **SPI reliability actually IMPROVES for seq3**: The current loop gives seq3
   a ~200 µs RDY window to land its SPI frame. The proposed loop gives ~725 µs.
   seq3 polls at ~1 ms intervals, so a larger window reduces the chance of
   missing a poll cycle. This means seq3's MIDI delivery becomes *more* reliable,
   not less.

4. **MIDI is event-based, not continuous**: Unlike PCM audio (which needs 1378
   frames/sec continuously), MIDI events are sparse. Missing one SPI frame just
   means the MIDI event arrives in the next frame (~1 ms later). PicoSeqRack
   already handles variable arrival timing gracefully.

5. **Codec timing is unchanged**: `Codec::WriteBuffer` and `Codec::ReadBuffer`
   are still called once per loop iteration, still process 32-sample blocks,
   still run at 44100/32 = 1378 Hz. The I2S clock drives the loop rate
   regardless of reordering.

##### Why a mode switch would be WORSE

A mode switch (e.g., `if (activePlugin == PicoAudioBridge) use_pipeline_order()`)
would introduce:

- **Two code paths** to test and maintain in the most timing-critical function
- **Race condition risk** during plugin switching — what happens if SetActivePlugin
  fires mid-loop and the flag changes between WriteBuffer and Process?
- **Complexity for zero benefit** — since the reorder is safe for all plugins,
  the conditional serves no purpose

The reorder is the right approach. It should be unconditional.

##### Verification for PicoSeqRack

After deploying the pipeline change, test with seq3:
- Play a sequence → verify all notes trigger correctly, no stuck notes
- Check audio quality → should be identical (or imperceptibly better due to
  improved SPI reliability)
- Monitor P4 debug output → `parse-err` count should not increase
- The +0.726 ms latency is below measurement threshold for manual testing

**PrepareResponse is unaffected**: The response packing (USB MIDI, waveforms,
Ableton Link data, LED status) runs before `QueueBuffer` in both the current and
proposed loop orders. seq3 reads these response fields — they will continue to be
populated correctly regardless of the reordering.

#### 2. P4-Side Resampling (PicoAudioBridge plugin changes) — ✅ NO IMPACT

Changes to `ctagSoundProcessorPicoAudioBridge.cpp` are **entirely internal**
to the PicoAudioBridge plugin. Other plugins are separate compilation units
with their own `Process()` methods. The changes:

- Adding a ring buffer inside PicoAudioBridge — private to the class
- Adding a resampler inside PicoAudioBridge — private to the class
- Detecting a new PCM magic (`PCM2` vs `PCM!`) — only PicoAudioBridge checks

**No shared code is modified.** PicoSeqRack never looks at PCM magic bytes.

#### 3. SpiProtocol.h Changes — ✅ NO CHANGES NEEDED

The `p4_spi_request2` struct does **not** need modification. The `synth_midi[256]`
field is already a generic byte buffer. Both MIDI bytes and PCM frames fit within
it. The PCM magic (`0x50434D21` or `0x50434D32`) is part of the payload data,
not part of the struct definition.

No changes to `SpiProtocol.h` are needed on either side. The struct remains
identical between seq3 and doom.

#### 4. rp2350_spi_stream Changes — ✅ NONE PROPOSED

The SPI slave driver (`rp2350_spi_stream.cpp`) is not being modified.
`QueueBuffer`, `GetReceivedBuffer`, and `GetSendBuffer` all remain unchanged.
The triple-buffering, handshake GPIO, and DMA transaction handling are untouched.

### Compatibility Checklist

| Change | Scope | PicoSeqRack Impact | PicoAudioBridge Impact | Other Plugins |
|--------|-------|-------------------|----------------------|---------------|
| Pipeline reorder | audio_task loop | +0.7ms latency (inaudible), larger RDY window (better) | Fixes throughput | +0.7ms latency |
| P4 resampler | PicoAudioBridge only | None | New capability | None |
| New PCM magic | PicoAudioBridge only | None | Backward compat | None |
| Ring buffer in plugin | PicoAudioBridge only | None | New capability | None |
| Remove RP2350 resampler | dada-tbd-doom only | None (different firmware) | Simpler ISR | None |

### What About Future RP2350 Firmware That Needs Both?

If a future RP2350 firmware needs to send both MIDI (for PicoSeqRack) and PCM
(for PicoAudioBridge) in the same session, the `synth_midi[256]` field already
supports this by protocol:

- If bytes 0-3 = `0x50434D21` → PicoAudioBridge interprets as PCM
- If bytes 0-3 are anything else → PicoSeqRack interprets as raw MIDI

The two plugins could even hypothetically coexist on `sp[0]` and `sp[1]` if
one were mono, though both currently declare `isStereo = true` (slot 0 only).

### Testing Plan

After implementing the pipeline fix on P4:

1. **Test with dada-tbd-doom**: Flash RP2350 doom firmware, verify sound quality
   improves (RDY LOW % should drop, frame rate should rise toward 1378 Hz)
2. **Test with tbd-pico-seq3**: Flash seq3 firmware, verify PicoSeqRack still
   responds to MIDI correctly, audio plays without glitches. Check that the
   extra 0.7 ms latency is not perceptible.
3. **Test plugin switching**: Use control SPI to switch between PicoAudioBridge
   and PicoSeqRack at runtime — verify both work correctly after the loop change.
4. **Test without any RP2350**: If `CONFIG_TBD_USE_RP2350` is disabled, the
   audio_task skips all SPI code. Verify standalone P4 operation is unaffected.

---

## P4-Side Fix: Pipeline SPI with Codec

### Concept

Move `QueueBuffer` (which sets RDY HIGH) **before** `Codec::WriteBuffer` so the
next SPI transfer runs in parallel with the codec's I2S DMA blocking wait:

```
CURRENT (sequential):                    PROPOSED (pipelined):
                                         
QueueBuf ──────┐                         Codec::WriteBuffer ──┐ 725µs
GetReceived ───┤ 364µs                   QueueBuf ────────────┤ (SPI runs during codec)
Process ───────┤ ~10µs                   GetReceived ──────── ┤ overlapped!
Codec::Read ───┤ ~10µs                   Process ─────────────┤ ~10µs
Codec::Write ──┤ 725µs                   Codec::Read ─────────┤ ~10µs
               │                                              │
Total: 1099µs (910 Hz)                   Total: max(725, 364)+30 ≈ 755µs (1325 Hz)
```

### Expected Result

- **Cycle time**: ~755 µs → **1325 Hz**
- Very close to the required 1378 Hz
- Additional tuning (reducing PrepareResponse overhead, eliminating debug
  printf) should close the remaining ~4% gap

### Implementation — Changes to SPManager::audio_task()

The current audio_task loop (simplified) looks like:

```cpp
while (runAudioTask) {
    // 1. Prepare response
    if (protocol.shouldPrepareNextResponse()) { ... }
    
    // 2. Queue response (sets RDY HIGH)
    if (protocol.shouldSendPreparedResponse()) {
        QueueBuffer(sendbuffer);    // ← currently here
        protocol.queuedPreparedResponse();
    }
    
    // 3. Check for received SPI data (blocks briefly)
    if (GetReceivedBuffer(&spi_request_ptr)) {
        // validate, extract MIDI, mark request seen
    }
    
    taskYIELD();
    
    // 4. Codec read + process + write
    Codec::ReadBuffer(finput, BUF_SZ);
    sp[0]->Process(pd);
    Codec::WriteBuffer(fbuf, BUF_SZ);  // blocks ~725µs
}
```

**Proposed change**: Move the QueueBuffer + GetReceivedBuffer block to just
before Codec::WriteBuffer, so the SPI slave transaction runs during the codec
blocking wait:

```cpp
while (runAudioTask) {
    // 1. Prepare response (same as before)
    if (protocol.shouldPrepareNextResponse()) { ... }
    
    // 2. Queue response (sets RDY HIGH)
    if (protocol.shouldSendPreparedResponse()) {
        QueueBuffer(sendbuffer);
        protocol.queuedPreparedResponse();
    }
    
    // 3. Codec write FIRST — blocks ~725µs, during which SPI transfer runs
    Codec::WriteBuffer(fbuf, BUF_SZ);
    
    // 4. Check for received SPI data (should be done by now)
    if (GetReceivedBuffer(&spi_request_ptr)) {
        // validate, extract MIDI, mark request seen
    }
    
    taskYIELD();
    
    // 5. Codec read + process
    Codec::ReadBuffer(finput, BUF_SZ);
    sp[0]->Process(pd);
}
```

### Key Considerations

1. **First iteration**: The codec write on the first loop iteration will write
   the zeroed `fbuf` (silence), which is fine — one block of silence at boot.

2. **SpiProtocolHelper state**: The state machine (`shouldPrepare` →
   `markPrepared` → `shouldSend` → `queued` → `markRequestSeen`) doesn't need
   changes. The ordering of prepare/queue vs receive/mark still works.

3. **RDY window**: With pipelining, RDY will be HIGH for 725 µs (the entire
   duration of `Codec::WriteBuffer` + some processing time), giving the RP2350
   ISR 3-4 opportunities to catch it per cycle.

4. **No changes needed on RP2350 side** — the timer ISR transport already
   handles variable RDY timing correctly.

### Validation

After deploying the P4 fix, check the RP2350 debug output:
```
[P4-SPI] sent=X isr=Y dma_busy=Z rdy_low=W rdy=1
```
- `rdy_low` percentage should drop from 72% to <30%
- `sent` / time should approach 1378 Hz
- Sound should be clean (test with 440 Hz test tone first)

### Zero-Latency Alternative: Split Codec Write

The pipeline reorder adds +0.726 ms because `Process(N)` output is written to
the codec in iteration N+1. This is inherent to the reorder — you can't have
`WriteBuffer` both before and after `Process` in the same iteration.

However, on a **single core** with **zero added latency**, we can split
`Codec::WriteBuffer` into two phases: **wait** (for previous DMA to finish) and
**start** (submit new buffer to DMA). The I2S DMA hardware is double-buffered —
`WriteBuffer` internally waits for a free slot, copies data, then the DMA sends
it asynchronously. By separating these phases:

```
CURRENT (sequential):                    SPLIT-PHASE (zero latency):

QueueBuf ──────┐                         PrepareResponse
GetReceived ───┤ 364µs                   QueueBuffer ──────┐ RDY HIGH
Process ───────┤ ~10µs                   WriteBufferWait ──┤ 725µs (prev DMA)
Codec::Read ───┤ ~10µs                     (SPI runs during wait!)
Codec::Write ──┤ 725µs                   GetReceivedBuffer ┤ SPI done
               │                         extract_midi ─────┤
Total: 1099µs                            Codec::ReadBuffer ┤ ~10µs
                                         Process ──────────┤ output → fbuf
                                         WriteBufferStart ─┤ starts DMA,
                                                           │ returns immediately
                                         Total: ~760µs (same throughput)
```

The key insight: `Process(N)` output goes to `WriteBufferStart(N)` in the
**same iteration**, and the DMA finishes during `WriteBufferWait(N+1)`. So the
audio is written with zero additional latency while SPI still overlaps with the
codec DMA wait.

```cpp
while (runAudioTask) {
    // 1. Prepare and queue SPI response
    if (protocol.shouldPrepareNextResponse()) { ... }
    if (protocol.shouldSendPreparedResponse()) {
        QueueBuffer(sendbuffer);       // RDY goes HIGH
        protocol.queuedPreparedResponse();
    }

    // 2. Wait for PREVIOUS I2S DMA to finish (~725µs)
    //    SPI transfer runs during this wait!
    Codec::WriteBufferWait();

    // 3. SPI transfer should be complete — get new data
    if (GetReceivedBuffer(&spi_request_ptr)) {
        // validate, extract MIDI, mark request seen
    }

    taskYIELD();

    // 4. Read input + process → output goes to fbuf
    Codec::ReadBuffer(finput, BUF_SZ);
    sp[0]->Process(pd);

    // 5. Start new I2S DMA with THIS iteration's output (returns immediately)
    Codec::WriteBufferStart(fbuf, BUF_SZ);
}
```

#### What This Requires

The `Codec` class needs a two-function API instead of the single `WriteBuffer`:

```cpp
// In Codec / I2S driver:
void WriteBufferWait();                    // blocks until DMA slot is free
void WriteBufferStart(float *buf, int n);  // copies to DMA buffer, starts transfer, returns

// The existing single call is equivalent to:
void WriteBuffer(float *buf, int n) {
    WriteBufferWait();       // ← this is the 725µs block
    WriteBufferStart(buf, n); // ← this returns quickly
}
```

If the existing ESP-IDF I2S driver uses `i2s_channel_write()`, the wait and
copy are already separable internally. The codec wrapper just needs to expose
them as two entry points.

#### Comparison

| | Simple pipeline reorder | Split-phase codec write | Dual-core split |
|--|--|--|--|
| Added latency | +0.726 ms | **0 ms** | **0 ms** |
| SPI throughput | ~1325 Hz | ~1325 Hz | ~2747 Hz |
| Code complexity | 5-line reorder | Codec driver change | New FreeRTOS task, queue |
| Thread safety | No concerns | No concerns | Shared state needs sync |
| Risk | Minimal | Low (codec driver mod) | Medium (concurrency) |

**Recommendation**: Try the simple pipeline reorder first (5 minutes to test,
0.726 ms is inaudible). If zero latency is desired, the split-phase codec write
is the cleanest single-core approach — it gives the same throughput improvement
with zero added latency, requiring only a small codec driver refactor.

The dual-core split (pinning SPI to core 1) is the nuclear option — highest
throughput (~2747 Hz, nearly 2× headroom) and zero latency, but requires
thread-safe access to the response buffer and protocol state.

---

## RP2350 Path Forward

### Current State — Optimal for Single-Buffer Protocol

The RP2350 timer ISR transport is as good as it can be with the current
single-buffer SPI protocol. The ISR:
- Fires 5000 times/sec (only 6% CPU overhead)
- Never blocks (checks DMA busy + RDY, skips if either says no)
- Catches any RDY HIGH window within 200 µs

### After P4 Pipeline Fix: Expected Performance

Once the P4 pipelines SPI with codec:
- RDY HIGH window: ~725 µs (up from ~200 µs)
- ISR catch rate: 3-4 chances per P4 cycle (up from ~1)
- Expected delivery: ~1300-1400 Hz (vs 678 Hz current)
- Sound quality: Should be clean with minimal dropout

### If Still Not Enough: Double-Buffering

If pipelining alone doesn't reach 1378 Hz, consider double-buffering on P4:

1. Queue two SPI slave transactions simultaneously (P4 already has 3 slots)
2. Process frame N while SPI receives frame N+1
3. Would decouple SPI receive latency from processing entirely
4. RDY could stay HIGH almost continuously

This requires changes to both `rp2350_spi_stream.cpp` (pre-queue 2 transactions)
and `SPManager.cpp` (separate receive from process timing).

### Recommendation: Move Resampling to P4

**This is the recommended next step after the pipeline fix.** Moving the
49716→44100 Hz resampling from the RP2350 ISR to the P4's PicoAudioBridge
plugin solves the throughput problem more completely than pipelining alone.

#### Why It Helps

Currently the RP2350 resamples 49716→44100 Hz *before* sending, packing 32
output samples (128 bytes) per frame. The `synth_midi[]` field is 256 bytes,
but only 136 are used (8-byte header + 128 bytes PCM).

If instead the RP2350 sends **raw 49716 Hz samples** without resampling, it
can pack more samples per frame:

| | Current (resample on RP2350) | Proposed (resample on P4) |
|--|------|---------|
| Samples per frame | 32 stereo (at 44100 Hz) | 62 stereo (at 49716 Hz) |
| Bytes per frame | 8 + 128 = 136 | 8 + 248 = 256 |
| Audio per frame | 0.726 ms | 1.247 ms |
| Codec blocks per frame | 1.0 | 1.72 |
| **Required SPI frame rate** | **1378 Hz** | **~800 Hz** |

At **800 Hz required**, even the *current* broken throughput of 678 Hz is only
15% short — and combined with the pipeline fix (~1325 Hz), there would be
**1.66× headroom**. The problem goes from "impossible" to "comfortable margin."

#### Why the P4 Is the Right Place to Resample

1. **CPU headroom**: The P4 runs at 400 MHz (dual-core RISC-V). The
   PicoAudioBridge `Process()` currently just copies 32 int16 samples to float
   — it has massive CPU to spare for a resampler.

2. **Quality**: The P4 could use a higher-quality algorithm (polyphase FIR,
   cubic interpolation) vs the RP2350's simple linear lerp. This would improve
   audio fidelity with zero cost on the constrained RP2350.

3. **Simpler ISR**: The RP2350 timer ISR would no longer maintain resampler
   state (`resample_frac` accumulator). `pab_pack_spi()` becomes a simple
   memcpy from the ring buffer — faster, no fractional arithmetic in ISR context.

4. **Decoupled sample rates**: If the source rate ever changes (different OPL
   clock, different game), only the P4 plugin config changes. The RP2350
   transport becomes sample-rate agnostic.

#### Implementation Sketch

**RP2350 changes** (`pico_audio_bridge.c`):
- `pab_pack_spi()`: Skip resampling, pack up to 62 raw stereo int16 samples
  from the ring buffer directly. Write actual sample count in header.
- Remove `RESAMPLE_STEP`, `resample_frac`, `input_needed()`.
- Update `PAB_SAMPLES_PER_FRAME` from 32 to 62 (or make it dynamic).
- Write source sample rate into the new header field (see protocol change below).

**P4 changes** (`ctagSoundProcessorPicoAudioBridge.cpp`):
- Add a small ring buffer (128–256 stereo pairs) inside the plugin.
- `Process()`: Push received samples into ring buffer. Check `source_rate`:
  - If `source_rate == 44100` (or 0): pull 32 samples directly, no resampling.
  - If `source_rate != 44100`: resample from `source_rate` → 44100 Hz to
    produce exactly 32 output samples per `Process()` call.
- Fixed-point linear interpolation is sufficient (same as current RP2350 code),
  or upgrade to cubic/polyphase for better quality.
- Persist resampler fractional state across `Process()` calls (class member).
- Recompute `resample_step` only when `source_rate` changes (cache last value).

**Protocol change** — PCM header v2 in `synth_midi[256]`:

Keep the existing `PCM!` magic for backward compatibility (pre-resampled 44100,
32 samples, no rate field). Introduce a new `PCM2` header that includes the
source sample rate, making P4-side resampling automatic and optional:

```
PCM! header (current, unchanged):        PCM2 header (new):
Offset  Size  Field                      Offset  Size  Field
0       4     0x50434D21 ("PCM!")         0       4     0x50434D32 ("PCM2")
4       4     sample_count: 32           4       2     sample_count (uint16, 1-62)
8       128   32 stereo int16 samples    6       2     source_rate_hz (uint16)
                                         8       N     N stereo int16 samples
                                                       (N = sample_count × 4 bytes)
```

`source_rate_hz` is the actual sample rate in Hz (e.g. 49716, 44100, 48000).
The uint16 range (0–65535) covers all practical audio sample rates.

The plugin logic becomes:

```cpp
void Process(ProcessData &pd) {
    uint32_t magic = *(uint32_t *)pd.midi_bytes;
    if (magic == 0x50434D21) {  // "PCM!"
        // Legacy: 32 pre-resampled 44100 Hz samples
        copy_int16_to_float(pd.midi_bytes + 8, pd.buf, 32);
    } else if (magic == 0x50434D32) {  // "PCM2"
        uint16_t count = *(uint16_t *)(pd.midi_bytes + 4);
        uint16_t rate  = *(uint16_t *)(pd.midi_bytes + 6);
        push_ring_buffer(pd.midi_bytes + 8, count);
        if (rate == 0 || rate == 44100) {
            // No resampling needed — pull 32 samples directly
            pull_ring_buffer(pd.buf, 32);
        } else {
            // Resample from source_rate → 44100
            resample_pull(pd.buf, 32, rate, 44100);
        }
    }
}
```

This means:
- If the RP2350 sends 44100 Hz (pre-resampled), it uses `PCM2` with
  `source_rate_hz = 44100` → plugin skips resampling, just buffers.
- If the RP2350 sends 49716 Hz (raw OPL), it uses `PCM2` with
  `source_rate_hz = 49716` → plugin resamples.
- Old firmware sending `PCM!` still works unchanged (backward compat).
- Any future source rate (48000 Hz, 32000 Hz) works without protocol changes.

#### Risk Assessment

- **Low risk**: The resampler is straightforward DSP — well-understood code.
- **One subtlety**: The number of raw input samples per frame varies slightly
  (61 or 62) depending on ring buffer fill level. The P4 plugin must handle
  variable-length input, which it already does via the sample count header field.
- **Fallback**: If something goes wrong, revert to the current approach by
  switching back to the old `PCM!` magic. Both modes coexist in the plugin.

### Potential RP2350 Improvements (Lower Priority)

These are not needed if the P4 pipeline fix + P4-side resampling work, but
could help in edge cases:

1. **Adaptive alarm period**: Monitor RDY LOW rate and adjust ALARM_PERIOD_US
   dynamically. If P4 is slow, reduce ISR frequency to save CPU.

2. **Ring buffer watermark**: If ring buffer runs low, the ISR could pack silence
   frames autonomously (currently `pab_pack_spi` handles this internally).

3. **DMA IRQ instead of timer poll**: Instead of polling DMA BUSY, use a DMA
   completion interrupt to immediately check RDY and send next frame. Would
   eliminate the 200 µs latency between DMA complete and next send attempt.

---

## Known Issues & Debugging

### PSRAM Init Hang
`stdio.h` MUST be the first `#include` in every `.c` file compiled for RP2350.
Including `stdbool.h` or SDK headers before `stdio.h` causes PSRAM init to hang
at "Initializing PSRAM on GPIO19...". Fixed files:
- `doom_stub.c`, `i_picosound.c`, `pico_audio_bridge.c`, `p4_spi_transport.c`
- `p4_control_link.c`, `pio_spi_oled.c`, `psram_init.c`, `sd_wad_loader.c`

### PSRAM Warm-Boot Hang
APS6404 PSRAM retains QPI state across soft resets. If PSRAM was in QPI mode
when reset occurs, the initialization sequence (which sends SPI-mode commands)
fails silently. Fix: Send Exit QPI (0xF5) + Reset Enable (0x66) + Reset (0x99)
+ 200 µs delay before any QPI init. Implemented in `psram_init.c` with 3 retries.

### P4 Debug Log `success=0`
The `transferSuccessCount++` line is **commented out** in the current P4 firmware
(`rp2350_spi_stream.cpp` lines 220, 281). The counter always reads 0 regardless
of whether frames actually arrive. Check `parse-err` and `tx-err` instead.

### Debug Output (Current Format)
RP2350 prints periodic stats every ~70 game frames:
```
[P4-SPI] sent=3390 isr=23775 dma_busy=3230 rdy_low=17155 rdy=0
```
- `sent`: total frames sent since boot
- `isr`: total ISR invocations
- `dma_busy`: ISR skips due to DMA still running
- `rdy_low`: ISR skips due to P4 not ready (main bottleneck indicator)
- `rdy`: current handshake GPIO state

### Analyzing ISR Debug Output

Use `tools/analyze_isr.py` to parse captured debug output:
```bash
python tools/analyze_isr.py < capture.log
```
Calculates rates, percentages, and identifies the dominant bottleneck.

---

## Files

### RP2350 side (`dada-tbd-doom/src/`)

| File | Purpose |
|------|---------|
| `p4_spi_transport.c` | TIMER1 ISR + DMA SPI1 master, direct register access |
| `p4_spi_transport.h` | Transport API (`init`, `poll`) |
| `p4_control_link.c` | SPI0 control channel (SetActivePlugin, etc.) |
| `p4_control_link.h` | Control link API |
| `pico_audio_bridge.c` | Ring buffer (2048 stereo pairs), raw PCM2 packer (no resampling), test tone |
| `pico_audio_bridge.h` | Bridge API, PCM2 magic (0x50434D32), ring buffer constants |
| `SpiProtocol.h` | Frame structs (p4_spi_request_header, p4_spi_request2) |
| `i_picosound.c` | Doom sound module → ring buffer → transport |
| `psram_init.c` | PSRAM warm-boot recovery (QPI exit + reset sequence) |

### P4 side (`dadamachines-ctag-tbd/`)

| File | Purpose |
|------|---------|
| `components/drivers/rp2350_spi_stream.cpp` | SPI2 slave, triple-buffered DMA, handshake GPIO |
| `components/drivers/rp2350_spi_stream.hpp` | Stream API (GetSendBuffer, QueueBuffer, GetReceivedBuffer) |
| `main/SPManager.cpp` | Audio task: receive frames, validate, dispatch to plugins |
| `main/SpiProtocol.h` | Shared frame structs (must match RP2350 side) |
| `main/SpiProtocolHelper.cpp` | CRC validation, sequence tracking, state machine |
| `components/ctagSoundProcessor/ctagSoundProcessorPicoAudioBridge.cpp` | PCM2 plugin: ring buffer (1024), cubic Hermite resampler, adaptive rate matching |
| `components/ctagSoundProcessor/ctagSoundProcessorPicoAudioBridge.hpp` | Plugin header: RING_SIZE, RING_WATERMARK, pcm2_active/primed flags |

### Tools

| File | Purpose |
|------|---------|
| `tools/analyze_isr.py` | Parse ISR debug output, compute rates and bottleneck |
| `tools/flash_and_capture.py` | Flash firmware and capture serial output |
| `tools/serial_capture.py` | Raw serial capture for debugging |
