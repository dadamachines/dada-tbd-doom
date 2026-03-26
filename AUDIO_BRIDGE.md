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
│ SFX Mixer + Fade    │──30 MHz──────► │  pd.midi_bytes[400]     │
│   (49716 Hz)        │  1378 Hz       │      │ PCM magic?       │
│         │           │  per frame     │      ▼                  │
│         ▼           │                │  int16 → float          │
│  Ring Buffer        │                │      │                  │
│  (49716 Hz int16)   │                │      ▼                  │
│         │           │                │  pd.buf (codec out)     │
│   Resampler         │                │                         │
│  49716→44100 Hz     │                └─────────────────────────┘
│         │           │
│  p4_spi_request2    │
│  .synth_midi[256]   │
└─────────────────────┘
```

## Data Flow — Per SPI Frame (~725 µs)

1. **Doom game loop** calls `I_Pico_UpdateSound()` which:
   - Calls the OPL music generator → fills buffer with int16 stereo at 49716 Hz
   - Mixes ADPCM SFX channels on top (ADPCM decompressed, resampled, stereo-panned)
   - Applies fade in/out
   - Pushes completed samples into a ring buffer

2. **SPI tick** (word-clock ISR ÷ 32 = 1378 Hz):
   - Reads 32 stereo samples from ring buffer, resampling 49716→44100 Hz
   - Packs them into `p4_spi_request2.synth_midi[]` with a PCM header
   - Computes CRC, fills `p4_spi_request_header`
   - DMA transfers 512-byte packet to P4

3. **P4 audio task** (`SPManager::audio_task`):
   - Receives SPI packet, validates header/CRC
   - Copies `synth_midi[]` → `ProcessData.midi_bytes[]`
   - Calls `PicoAudioBridge::Process(pd)`
   - Plugin detects PCM magic, extracts 32 stereo int16 samples
   - Converts to float [-1.0, +1.0], writes to `pd.buf`
   - Codec outputs audio via I2S

## SPI Protocol

The real-time SPI uses the same framing as tbd-pico-seq3:

```
DMA buffer (1024 bytes, master sends):
  [0..1]   0xCA 0xFE (fingerprint, stripped by rp2350_spi_stream)
  [2..17]  p4_spi_request_header (magic, sequence, CRC)
  [18..]   p4_spi_request2 (payload)

P4 slave receives 512 bytes (STREAM_BUFFER_SIZE_).
```

### PCM Payload in `synth_midi[256]`

```
Offset  Size  Field
0       4     PCM magic: 0x50434D21 ("PCM!")
4       4     Sample count (32)
8       128   32 interleaved stereo int16 samples (L0 R0 L1 R1 ...)
136     120   Unused (zero)
```

Total: 136 bytes of 256 available. 128 bytes of audio per frame =
32 × 2 channels × 2 bytes = ~176 KB/s, trivial for 30 MHz SPI.

## Resampling

Doom with `USE_EMU8950_OPL=1` generates audio at **49716 Hz** (native OPL clock
÷ 72). The TBD codec runs at **44100 Hz**. The ring buffer stores 49716 Hz
samples; the SPI reader uses fixed-point linear interpolation:

- Step = 49716 × 65536 / 44100 ≈ 73635
- Per frame: reads ~36 input samples, outputs 32 samples at 44100 Hz

## P4 Plugin: PicoAudioBridge

- Stereo plugin (output only, no input processing)
- No CV / TRIG (same pattern as PicoSeqRack)
- `Process()`: reads PCM from `data.midi_bytes`, converts int16→float, writes `data.buf`
- If no PCM magic detected, outputs silence (graceful fallback)
- Registered via `SetActivePlugin(0, "PicoAudioBridge")` from RP2350 at boot

## Files

### P4 side (`dadamachines-ctag-tbd`)
- `components/ctagSoundProcessor/ctagSoundProcessorPicoAudioBridge.hpp`
- `components/ctagSoundProcessor/ctagSoundProcessorPicoAudioBridge.cpp`
- `sdcard_image/data/sp/mp-PicoAudioBridge.json`
- No CMake changes needed (file GLOB auto-detects)

### RP2350 side (`dada-tbd-doom`)
- `src/doom_audio.h` — ring buffer types, PCM transport defines
- `src/doom_audio.c` — ring buffer + resampler + SPI request packing
- `src/SpiProtocol.h` — p4_spi_request/response structs
- `src/i_picosound_stub.c` — real sound module (replaces stubs)
- `src/Midi.h` / `src/Midi.cpp` — audio-aware SPI packing
- `platformio.ini` — build filter additions
