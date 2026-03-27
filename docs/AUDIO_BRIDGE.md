# TBD-16 Doom Audio Bridge

## Overview

The TBD-16 runs Doom on the RP2350 co-processor, but audio output is handled by
the ESP32-P4 main processor through its TLV320AIC3254 codec (I2S, 44100 Hz,
32-bit stereo, 32-sample blocks). This document describes how Doom's audio
reaches the P4 and plays through the codec.

## Architecture

```
 RP2350 (Doom Engine)                   ESP32-P4 (Audio Engine)
┌─────────────────────┐                ┌─────────────────────────┐
│ EMU8950 OPL Music   │                │                         │
│   (49716 Hz)        │                │  PicoAudioBridge Plugin │
│         │           │                │    Process(pd)          │
│         ▼           │   SPI1 DMA     │      │                  │
│ SFX Mixer + Fade    │──25 MHz──────► │  pd.midi_bytes[400]     │
│   (49716 Hz)        │  512B frames   │      │ PCM magic?       │
│         │           │                │      ▼                  │
│         ▼           │                │  int16 → float          │
│  Ring Buffer        │                │      │                  │
│  (2048 stereo pairs)│   Handshake    │      ▼                  │
│         │           │ ◄── GPIO 22 ── │  pd.buf (codec out)     │
│   Resampler         │   (RDY HIGH)   │                         │
│  49716→44100 Hz     │                └─────────────────────────┘
│         │           │
│  p4_spi_request2    │   Control SPI (spi0)
│  .synth_midi[256]   │──────────────► SetActivePlugin
└─────────────────────┘                "PicoAudioBridge"
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
  (ready to receive). RP2350 polls GPIO 22 before each DMA transfer.
- **Triple-buffered** on P4 side (3 DMA transaction slots)

### Link2 — Control (SPI0 ↔ SPI3_HOST)

| Signal    | RP2350 GPIO | P4 GPIO | Direction |
|-----------|-------------|---------|-----------|
| MISO      | 32          | 22      | P4 → RP   |
| CS        | 33          | 20      | RP → P4   |
| SCLK      | 34          | 21      | RP → P4   |
| MOSI      | 35          | 23      | RP → P4   |

- 2048-byte frames, byte-by-byte (no DMA), used for `SetActivePlugin` etc.

## Data Flow — Per SPI Frame

1. **Doom game loop** calls `I_Pico_UpdateSound()` which:
   - Calls the OPL music generator → fills buffer with int16 stereo at 49716 Hz
   - Mixes ADPCM SFX channels on top (decompressed, resampled, stereo-panned)
   - Applies fade in/out
   - Pushes completed samples into a ring buffer (2048 stereo pairs)

2. **`p4_spi_transport_poll()`** (called from sound update, ≤15 ms budget):
   - Polls P4 handshake GPIO 22 (active HIGH = P4 ready)
   - `pab_pack_spi()` reads 32 stereo samples from ring buffer,
     resampling 49716→44100 Hz with fixed-point linear interpolation
   - Packs PCM into `p4_spi_request2.synth_midi[]` with 0x50434D21 magic
   - Fills `p4_spi_request_header` (0xCAFE magic, sequence counter, CRC)
   - DMA transfers 512-byte frame to P4 via SPI1
   - Waits for DMA completion + 15 µs post-transfer delay
   - Repeats until time budget exhausted or P4 not ready

3. **P4 `SPManager::audio_task()`**:
   - `GetReceivedBuffer()` checks for completed SPI slave transaction
   - Validates 0xCAFE watermark (bytes 0-1 of raw buffer)
   - Validates CRC via `SpiProtocolHelper::validateRequestPacket()`
   - Copies `synth_midi[]` → `ProcessData.midi_bytes[]`
   - Calls `PicoAudioBridge::Process(pd)`
   - Plugin detects 0x50434D21 magic, extracts 32 stereo int16 samples
   - Converts int16 → float (÷ 32768), writes to `pd.buf[]`
   - Codec outputs audio via I2S at 44100 Hz

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

```
Offset  Size  Field
0       4     PCM magic: 0x50434D21 ("PCM!")
4       4     Sample count: 32
8       128   32 interleaved stereo int16 samples (L0 R0 L1 R1 ...)
136     120   Unused (zero)
```

Total: 136 bytes of 256 available. 128 bytes of audio per frame =
32 × 2 channels × 2 bytes = ~176 KB/s at full rate.

## Resampling

Doom with `USE_EMU8950_OPL=1` generates audio at **49716 Hz** (native OPL clock
÷ 72). The TBD codec runs at **44100 Hz**. The ring buffer stores 49716 Hz
samples; the SPI packer uses fixed-point linear interpolation:

- Step = 49716 × 65536 / 44100 ≈ 73635 (16.16 fixed point)
- Per frame: reads ~36 input samples, outputs 32 samples at 44100 Hz
- Ring buffer: 2048 stereo pairs (power of 2 for fast masking)

## P4 Plugin: PicoAudioBridge

- Stereo plugin (output only, no input processing)
- No CV / TRIG (dummy entries only)
- `Process()`: reads PCM from `data.midi_bytes`, converts int16→float, writes `data.buf`
- If no PCM magic detected, outputs silence (graceful fallback)
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

## DMA Transport (RP2350 side)

The SPI1 DMA transport uses direct register access (no SDK headers) to avoid
the RP2350 PSRAM init hang caused by `stdbool.h` include ordering.

- **DMA channels**: 4 (TX) and 5 (RX), hard-assigned
- **CTRL register**: CHAIN_TO set to self (disables spurious chaining)
- **TREQ_SEL**: DREQ_SPI1_TX=26, DREQ_SPI1_RX=27 (bits [22:17])
- **SPI DMACR**: offset 0x24, RXDMAE | TXDMAE must be enabled
- **Transfer sequence** (matches DaDa_SPI exactly):
  1. Wait previous DMA done (poll BUSY bit 26)
  2. Poll handshake GPIO 22 (500 µs timeout)
  3. Pack frame (header + CRC + PCM payload)
  4. Configure TX+RX channels via AL1_CTRL (no trigger)
  5. Start both simultaneously via MULTI_CHAN_TRIGGER (0x450)
  6. Wait DMA completion
  7. 15 µs post-transfer delay

## Known Issues & Debugging

### PSRAM Init Hang
`stdio.h` MUST be the first `#include` in every `.c` file compiled for RP2350.
Including `stdbool.h` or SDK headers before `stdio.h` causes PSRAM init to hang
at "Initializing PSRAM on GPIO19...". Fixed files:
- `doom_stub.c`, `i_picosound.c`, `pico_audio_bridge.c`, `p4_spi_transport.c`
- `p4_control_link.c`, `pio_spi_oled.c`, `psram_init.c`, `sd_wad_loader.c`

### P4 Debug Log `success=0`
The `transferSuccessCount++` line is **commented out** in the current P4 firmware
(`rp2350_spi_stream.cpp` lines 220, 281). The counter always reads 0 regardless
of whether frames actually arrive. Check `parse-err` and `tx-err` instead.

### Debug Output
RP2350 prints periodic stats:
```
[P4-SPI] sent=1234 this=5 rdy=1 sr=0x03
```
- `sent`: total frames sent since boot
- `this`: frames sent in the last poll cycle
- `rdy`: current handshake GPIO state (1 = P4 ready)
- `sr`: SPI status register (bit 1=TNF, bit 2=RNE, bit 4=BSY)

## Files

### RP2350 side (`dada-tbd-doom/src/`)

| File | Purpose |
|------|---------|
| `p4_spi_transport.c` | DMA SPI1 master, direct register access, DaDa_SPI pattern |
| `p4_spi_transport.h` | Transport API (`init`, `poll`) |
| `p4_control_link.c` | SPI0 control channel (SetActivePlugin, etc.) |
| `p4_control_link.h` | Control link API |
| `pico_audio_bridge.c` | Ring buffer, resampler (49716→44100), PCM packer, test tone |
| `pico_audio_bridge.h` | Bridge API, PCM magic, ring buffer constants |
| `SpiProtocol.h` | Frame structs (p4_spi_request_header, p4_spi_request2) |
| `i_picosound.c` | Doom sound module → ring buffer → transport |

### P4 side (`dadamachines-ctag-tbd/`)

| File | Purpose |
|------|---------|
| `components/drivers/rp2350_spi_stream.cpp` | SPI2 slave, triple-buffered DMA, handshake GPIO |
| `components/drivers/rp2350_spi_stream.hpp` | Stream API |
| `main/SPManager.cpp` | Audio task: receive frames, validate, dispatch to plugins |
| `main/SpiProtocol.h` | Shared frame structs (must match RP2350 side) |
| `main/SpiProtocolHelper.cpp` | CRC validation, sequence tracking |
| `components/ctagSoundProcessor/ctagSoundProcessorPicoAudioBridge.cpp` | PCM plugin |
