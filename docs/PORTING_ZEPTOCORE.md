# Porting _core (Zeptocore) to TBD-16

## What is _core?

[_core](https://github.com/schollz/_core) by Zack Scholl is a sample-based instrument / creative sampler firmware for RP2040 boards. It powers the **zeptocore**, **ectocore**, and **ezeptocore** hardware, providing:

- 16-voice polyphonic WAV sample playback from SD card
- 16 real-time audio effects (saturation, fuzz, delay, comb filter, beat repeat, resonant filter, freeverb reverb, bitcrush, etc.)
- Fixed-point Q16.16 DSP — entirely integer math, no FPU required
- Tape-style scratching, time-stretching, granular synthesis
- Step sequencer with per-step probability, retrigger, and swing
- MIDI in/out support (clock, notes, CC)
- Direct button/knob control with visual feedback

The TBD-16 is a strong candidate for running _core because the hardware is remarkably similar: same class of microcontroller (RP2350 vs RP2040), comparable UI layout (buttons, knobs, OLED), I2S audio output path, SD card, and MIDI ports.

This document covers the porting strategy, focusing on the **critical SD card challenge** — _core reads WAV samples continuously during playback, which requires a fundamentally different SD access pattern than our current boot-only Doom loader.

> **Companion document**: See [TBD16_AUDIO_DRIVER.md](TBD16_AUDIO_DRIVER.md) for the complete audio transport architecture and drop-in driver code.

---

## Hardware Comparison

### What Maps Directly

| Feature | Zeptocore | TBD-16 | Compatibility |
|---------|-----------|--------|---------------|
| MCU | RP2040 (133 MHz dual Cortex-M0+) | RP2350B (150 MHz dual Cortex-M33) | RP2350 is faster, superset |
| Audio output | PCM5102 DAC via I2S (PIO) | TLV320AIC3254 via SPI→P4→I2S | Drop-in driver available (see TBD16_AUDIO_DRIVER.md) |
| Sample rate | 44100 Hz stereo int16 | 44100 Hz stereo int16 | Exact match |
| OLED | SSD1306 128×64 (I2C/SPI) | SSD1309 128×64 (PIO SPI) | Same resolution, same protocol |
| Step buttons | 16 buttons (direct GPIO) | 16 buttons (D1–D16 via STM32 I2C) | Identical count and layout |
| Knobs | 3 pots (direct ADC) | 5 pots + 2 side pots (STM32 I2C) | More knobs available |
| Function buttons | 3 (direct GPIO) | 8+ (F1, F2, F5, F6, S1, S2, Play, Rec, Master, Sound) | More available |
| Directional | None | 4-way D-pad | Extra input for _core |
| SD card | SPI or SDIO | SDIO (4-bit, PIO) | Upgrade — much faster |
| MIDI | UART (1 in, 1 out) | 2× UART (2 in, 2 out) | More MIDI ports |
| Neopixel | Optional | GPIO 26 | Available |
| RAM | 264 KB SRAM | 520 KB SRAM + 8 MB PSRAM | Much more memory |

### What's Different

| Feature | Zeptocore | TBD-16 | Impact |
|---------|-----------|--------|--------|
| DAC connection | Direct I2S from RP2040 | Routed through ESP32-P4 | Use tbd16_audio driver |
| Button interface | Direct GPIO reads | I2C from STM32 at 0x42 | Abstraction layer needed |
| Knob interface | Direct ADC reads | I2C from STM32 | Abstraction layer needed |
| Accelerometer | None | 3-axis via STM32 | Bonus — can map to effects |
| Clock system | 200–225 MHz (overclocked) | 150 MHz (stock) | RP2350 Cortex-M33 is faster per-clock |
| USB audio | Optional | Through P4 | Different path |

---

## The SD Card Challenge

This is the make-or-break issue for porting _core to TBD-16.

### How _core Uses the SD Card

_core reads WAV sample data **continuously during audio playback** — not just at boot. Inside the audio callback running on Core 1, every 441-sample block triggers:

```
┌─────────────────────────────────────────────────────────┐
│ Audio Callback (Core 1, every ~10 ms)                   │
│                                                         │
│  for each active voice (up to 16):                      │
│    f_lseek(fp, sample_position)    ← SD card seek       │
│    f_read(fp, buf, chunk_size)     ← SD card read       │
│    apply_effects(buf)              ← DSP chain          │
│    mix_into_output(buf)            ← accumulate         │
│                                                         │
│  give_audio_buffer(output)         ← send to DAC        │
└─────────────────────────────────────────────────────────┘
```

This means the SD card must be available and fast enough to service multiple file reads per audio block, every 10 ms, without glitching.

### Why We Can't Buffer Everything in PSRAM

With 8 MB of PSRAM, you might think we could just preload all samples at boot (like Doom loads its WAD). But _core projects can contain **gigabytes** of samples — users load entire drum kits, field recordings, and long audio files. 8 MB is not enough.

Even a modest _core project with 16 banks × 16 samples × 5 seconds each at 44100 Hz stereo = **~540 MB**. The SD card must remain the primary storage.

### What PSRAM *Can* Do

PSRAM is still valuable as a **cache layer**:

- **Effect buffers**: Delay lines (up to 4 seconds = ~700 KB stereo), reverb buffers, comb filter history — all currently limited by SRAM can expand into PSRAM
- **Read-ahead cache**: Pre-buffer the next N chunks of each active voice into PSRAM, so SD reads happen asynchronously ahead of the audio callback
- **Sample header cache**: Keep the first few KB of frequently-used samples in PSRAM for instant note-on response
- **Sequencer state**: Pattern data, probability tables, etc. — small but benefits from fast access

---

## The Solution: 4-Bit SDIO via PIO

### TBD-16 SD Card Wiring (Perfect SDIO Match)

The TBD-16 PCB wires the SD card with **all six SDIO signals** to the RP2350:

| Signal | RP2350 GPIO | Function |
|--------|-------------|----------|
| CLK | GPIO 2 | SDIO clock |
| CMD | GPIO 3 | SDIO command/response |
| D0 | GPIO 4 | Data bit 0 |
| D1 | GPIO 5 | Data bit 1 |
| D2 | GPIO 6 | Data bit 2 |
| D3 | GPIO 7 | Data bit 3 |
| DET | GPIO 8 | Card detect (active low) |
| PWR | GPIO 17 | Power enable |

This matches the `no-OS-FatFS-SD-SDIO-SPI-RPi-Pico` library's PIO SDIO requirements **exactly**:

```
Library requirement:        TBD-16 wiring:
  CLK = D0 - 2               GPIO 2 = GPIO 4 - 2  ✓
  D1  = D0 + 1               GPIO 5 = GPIO 4 + 1  ✓
  D2  = D0 + 2               GPIO 6 = GPIO 4 + 2  ✓
  D3  = D0 + 3               GPIO 7 = GPIO 4 + 3  ✓
  CMD = any                   GPIO 3               ✓
```

### Why SDIO Solves the Problem

**1. No SPI0 conflict.** SDIO uses PIO, not hardware SPI. SPI0 (GPIO 32–35) stays free for the P4 control link. SPI1 (GPIO 28–31) stays free for audio transport. No pin sharing, no time-division multiplexing.

```
Current Doom:                       _core on TBD-16:
┌──────────────────────┐            ┌──────────────────────┐
│ Boot:                │            │ Always:              │
│   SPI0 → SD card     │            │   PIO1 → SD card     │  (SDIO)
│   (GPIO 2-4,7)       │            │   (GPIO 2-7)         │
│                      │            │                      │
│ After boot:          │            │   SPI0 → P4 link     │  (always)
│   SPI0 → P4 link     │            │   (GPIO 32-35)       │
│   (GPIO 32-35)       │            │                      │
│   SD card released   │            │   SPI1 → P4 audio    │  (always)
└──────────────────────┘            │   (GPIO 28-31)       │
                                    └──────────────────────┘
```

**2. Much faster.** From the library's benchmarks at 125 MHz `clk_sys`:

| Metric | SPI | 4-bit SDIO | Improvement |
|--------|-----|------------|-------------|
| Read throughput | 2.75 MB/s | 12.3 MB/s | **4.5×** |
| Write throughput | 2.92 MB/s | 10.5 MB/s | **3.6×** |

At 150 MHz (TBD-16 stock clock), SDIO throughput will be even higher. _core needs to read at most 16 voices × 44100 Hz × 2 channels × 2 bytes = **2.76 MB/s peak** — well within SDIO's capability, but dangerously close to SPI's limit.

**3. The library is already in the project.** `lib/no-OS-FatFS-SD-SDIO-SPI-RPi-Pico` is already a dependency. It supports SDIO mode via PIO out of the box. The `examples/PlatformIO/one_SDIO/` example shows exactly how to configure it.

### PIO Resource Allocation

RP2350 has three PIO blocks. Current allocation:

| PIO Block | Current Use | _core Use |
|-----------|-------------|-----------|
| PIO0 | OLED SPI (SM0) | OLED SPI (SM0) — unchanged |
| PIO1 | Free | **SD card SDIO** |
| PIO2 | Free | Free (available for future use) |

No conflicts. The SDIO driver uses two DMA channels (claimed dynamically) and one DMA IRQ.

---

## hw_config.c: SPI → SDIO Migration

### Before (Current Doom SPI Mode)

```c
static spi_t spi = {
    .hw_inst = spi0,
    .sck_gpio = 2,
    .mosi_gpio = 3,
    .miso_gpio = 4,
    .baud_rate = 125 * 1000 * 1000 / 4,  // 31.25 MHz
};

static sd_spi_if_t spi_if = {
    .spi = &spi,
    .ss_gpio = 7,
};

static sd_card_t sd_card = {
    .type = SD_IF_SPI,
    .spi_if_p = &spi_if,
    .use_card_detect = true,
    .card_detect_gpio = 8,
    .card_detected_true = 0,
    .card_detect_use_pull = true,
    .card_detect_pull_hi = true,
};
```

### After (SDIO via PIO1)

```c
static sd_sdio_if_t sdio_if = {
    .CMD_gpio = 3,
    .D0_gpio  = 4,      // D1=5, D2=6, D3=7 set implicitly
                         // CLK=2 set implicitly (D0 - 2)
    .SDIO_PIO = pio1,   // PIO1 — PIO0 is used by OLED
    .DMA_IRQ_num = DMA_IRQ_1,  // DMA_IRQ_0 may be used by audio
    .baud_rate = 150 * 1000 * 1000 / 4,  // 37.5 MHz max at 150 MHz clk_sys
};

static sd_card_t sd_card = {
    .type = SD_IF_SDIO,
    .sdio_if_p = &sdio_if,
    .use_card_detect = true,
    .card_detect_gpio = 8,
    .card_detected_true = 0,
    .card_detect_use_pull = true,
    .card_detect_pull_hi = true,
};
```

Key changes:
- `SD_IF_SPI` → `SD_IF_SDIO`
- No more `spi_t` or `spi0` — SDIO uses PIO, not hardware SPI
- `spi_if_p` → `sdio_if_p`
- `SDIO_PIO = pio1` to avoid conflicting with OLED on PIO0
- Baud rate can go higher (up to `clk_sys / 4`)

The `sd_sdio_ctor` stub in the current hw_config.c must be removed — the library's real implementation will be linked instead.

---

## Audio Integration

### Zero-Change pico_audio_i2s Shim

TBD-16 provides a **drop-in `pico_audio_i2s` implementation** so _core's audio code compiles and runs without modification. The shim header (`pico/audio_i2s.h`) and implementation (`pico_audio_i2s_shim.c`) provide exactly the same API that pico-extras exposes:

| Function | What it does on TBD-16 |
|---|---|
| `audio_new_producer_pool()` | Allocates N buffers (default 3 × 441 samples) |
| `audio_i2s_setup()` | Stores sample rate, forwards to PAB (pin/PIO args ignored) |
| `audio_i2s_connect()` | Initializes the PAB ring buffer |
| `audio_i2s_set_enabled()` | Enables/disables audio flow |
| `take_audio_buffer()` | Returns a free buffer, with backpressure from the PAB ring |
| `give_audio_buffer()` | Copies samples into the PAB ring buffer → SPI → P4 codec |

_core's existing `init_audio()` and `i2s_callback_func()` work as-is:

```c
// _core's audio init — UNCHANGED
audio_buffer_pool_t *init_audio() {
    audio_format_t af = { .pcm_format = AUDIO_PCM_FORMAT_S16,
                          .sample_freq = 44100, .channel_count = 2 };
    audio_buffer_format_t abf = { .format = &af, .sample_stride = 4 };
    audio_buffer_pool_t *pool = audio_new_producer_pool(&abf, 3, SAMPLES_PER_BUFFER);

    audio_i2s_config_t config = { .data_pin = ..., .clock_pin_base = ...,
                                  .dma_channel = 0, .pio_sm = 0 };
    audio_i2s_setup(&af, &af, &config);  // pin/PIO ignored on TBD-16
    audio_i2s_connect(pool);
    audio_i2s_set_enabled(true);
    return pool;
}

// _core's audio callback (Core 1) — UNCHANGED
void i2s_callback_func() {
    audio_buffer_t *buffer = take_audio_buffer(ap, false);
    if (!buffer) return;
    int16_t *samples16 = (int16_t *)buffer->buffer->bytes;
    // ... DSP processing, SD reads, effects ...
    buffer->sample_count = buffer->max_sample_count;
    give_audio_buffer(ap, buffer);
}
```

### How the Audio Path Works

```
┌──────────────────────────────────────────────────────────────────┐
│  _core code (unchanged)                                          │
│  take_audio_buffer → fill → give_audio_buffer                    │
│         │                          │                             │
│  (gets free pool buf)    (copies into PAB ring buffer)           │
└──────────────────────── ─ ─ ─ ─ ─ ─ ─ ─ ─ ─ ─ ─ ─ ─ ─ ─ ─ ─ ─┘
         ↓                          ↓
┌──────────────────────────────────────────────────────────────────┐
│  PAB ring buffer (2048 stereo pairs, SPSC lock-free)             │
│  TIMER1 ISR → pab_pack_spi() → SPI1 DMA → ESP32-P4              │
│                                    → TLV320AIC3254 DAC → output  │
└──────────────────────────────────────────────────────────────────┘
```

### Sample Rate

The shim automatically sets the PAB source rate from `audio_i2s_setup()`:
- _core: 44100 Hz → identity pass-through on P4
- Doom: 49716 Hz → P4 resamples via cubic Hermite to 44100 Hz

### Output Level Gotcha

The P4's PicoAudioBridge applies **×8 linear gain** to compensate for Doom's quiet output. _core generates near-full-scale int16 samples. Without attenuation, output will clip hard:

```
_core output:  ±32000  × 8 gain = ±256000  → clipped to ±32767

Fix: shift right by 3 (divide by 8)
_core output:  ±32000 >> 3 = ±4000  × 8 gain = ±32000  → clean
```

This attenuation can be applied inside `i2s_callback_func()` before writing to the buffer, or the gain can be reduced in the PicoAudioBridge plugin config on the P4 side.

---

## Input Mapping

### _core's Direct GPIO → TBD-16's I2C Abstraction

_core reads buttons and knobs directly from RP2040 GPIOs. On TBD-16, the STM32 UI board handles all input and provides it over I2C1 at address 0x42.

Create a thin hardware abstraction:

```c
// hw_input.h — abstract _core's input layer

#include <stdint.h>

typedef struct {
    uint16_t knob[5];        // 10-bit pot values (0–1023)
    uint16_t step_buttons;   // D1–D16 as bitmask
    uint8_t  func_buttons;   // F1, F2, etc.
    uint16_t nav_buttons;    // D-pad, Play, Rec, etc.
    int16_t  accel[3];       // accelerometer (bonus)
} hw_input_t;

void hw_input_init(void);
void hw_input_read(hw_input_t *input);
```

### Button Mapping

| _core Function | Zeptocore Button | TBD-16 Button | MCL Bit |
|----------------|------------------|---------------|---------|
| Step 1–16 | Direct GPIO buttons | D1–D16 | `d_btns` bits 0–15 |
| Bank select | Knob 1 | POT1 | `pot_adc_values[0]` |
| Tempo | Knob 2 | POT2 | `pot_adc_values[1]` |
| Volume / FX depth | Knob 3 | POT3 | `pot_adc_values[2]` |
| Play/pause | Mode button | PLAY | `mcl_btns` bit 8 |
| Record | — | REC | `mcl_btns` bit 9 |
| Shift | — | S1 / S2 | `mcl_btns` bits 10–11 |
| Menu navigate | — | D-pad ↑↓←→ | `mcl_btns` bits 0–3 |
| Bank A / B | — | F5 / F6 | `mcl_btns` bits 4–5 |
| Mode toggle | — | Master / Sound | `mcl_btns` bits 6–7 |

The TBD-16 has **more** input than zeptocore — extra knobs, D-pad, function buttons, and an accelerometer — which could be mapped to _core's effects parameters.

---

## Multicore Strategy

_core uses both cores:

| Core | _core (RP2040) | TBD-16 (_core port) |
|------|----------------|---------------------|
| Core 0 | USB, I/O, DMA ISR, random | USB (via P4), I2C UI polling, random |
| Core 1 | Audio callback (SD read + DSP) | Audio callback (SD read + DSP) |

This maps naturally. The key change is that Core 0's I/O moves from direct GPIO to I2C polling of the STM32 UI board.

### ISR Allocation

| ISR | Purpose | Core |
|-----|---------|------|
| TIMER1 (5 kHz) | Audio transport to P4 | Core 0 |
| DMA IRQ 1 | SDIO card transfers | Core 1 |
| I2C1 (polled) | UI board reads | Core 0 |

---

## MIDI Support

The TBD-16 has **two full MIDI ports**, more than _core's single port:

| Port | TX GPIO | RX GPIO | UART | Suggested Use |
|------|---------|---------|------|---------------|
| MIDI-1 | GPIO 36 | GPIO 37 | UART1 | Main MIDI in/out (clock, notes) |
| MIDI-2 | GPIO 44 | GPIO 45 | UART0 | Secondary (CC, program change) |

_core already supports MIDI clock sync, note on/off, and CC mapping. The second port is a bonus — could carry MIDI thru or separate CC channels.

---

## Complete Porting Checklist

### Phase 1: Audio + SD (Minimum Viable)

- [ ] Switch hw_config.c from SPI to SDIO (see section above)
- [ ] Remove Petit FatFS dependency, use full FatFS from no-OS-FatFS library
- [ ] Replace `init_audio()` with `tbd16_audio_init()`
- [ ] Replace `take_audio_buffer/give_audio_buffer` with `tbd16_audio_write()`
- [ ] Add `>>3` attenuation before `tbd16_audio_write()`
- [ ] Test: single WAV file plays from SD through speakers

### Phase 2: Input

- [ ] Implement `hw_input.h` I2C abstraction
- [ ] Map D1–D16 step buttons to _core's button array
- [ ] Map POT1–POT3 to _core's three knob functions
- [ ] Map PLAY to _core's play/pause
- [ ] Test: button presses trigger samples, knobs control effects

### Phase 3: Display

- [ ] Adapt _core's SSD1306 OLED code to SSD1309 + PIO SPI
- [ ] Update frame buffer push to use `pio_spi_oled_write()` instead of I2C
- [ ] The display is same resolution (128×64) — no layout changes needed

### Phase 4: MIDI

- [ ] Initialize UART1 on GPIO 36/37 for MIDI-1
- [ ] Wire _core's `midi_note_on/off/cc` callbacks to UART RX
- [ ] Wire _core's MIDI clock output to UART TX
- [ ] Optional: use UART0 (GPIO 44/45) for MIDI-2

### Phase 5: Polish

- [ ] Map extra knobs (POT4, POT5) to additional _core parameters
- [ ] Map D-pad to bank/mode navigation
- [ ] Map accelerometer to effect modulation (tilt = filter cutoff, etc.)
- [ ] Use PSRAM for extended delay lines and read-ahead cache
- [ ] Performance tuning: SDIO baud rate, read-ahead buffer size

---

## SD Card Read-Ahead Strategy

For glitch-free audio, SD reads should ideally not block the audio callback. A read-ahead scheme using PSRAM can help:

```
┌─────────────────────────────────────────────────────────────┐
│ Core 0: Read-Ahead Task                                     │
│                                                             │
│  for each active voice:                                     │
│    if (psram_cache[voice] < THRESHOLD):                     │
│      f_read(fp, psram_cache[voice].buf, CHUNK)  ← SD read  │
│      psram_cache[voice].ready += CHUNK                      │
│                                                             │
│  (runs continuously, fills PSRAM cache from SD)             │
└─────────────────────────────────────────────────────────────┘

┌─────────────────────────────────────────────────────────────┐
│ Core 1: Audio Callback                                      │
│                                                             │
│  for each active voice:                                     │
│    memcpy(buf, psram_cache[voice].buf, block_size) ← fast! │
│    apply_effects(buf)                                       │
│    mix_into_output(buf)                                     │
│                                                             │
│  tbd16_audio_write(output, 441)                             │
└─────────────────────────────────────────────────────────────┘
```

This decouples SD latency from audio timing. PSRAM reads are deterministic (~150 ns per 32-bit word at QMI speeds), while SD card reads can vary by milliseconds.

**Cache budget**: With 8 MB PSRAM, you could cache ~4 seconds of 16-voice stereo audio (16 × 44100 × 4 bytes × 4 seconds ≈ 11 MB — so more like 2–3 seconds). That's plenty of buffer to absorb SD card latency spikes.

---

## Known Challenges

### 1. FatFS Thread Safety

_core reads files from Core 1 while Core 0 might browse the filesystem (listing samples, loading new banks). FatFS's thread safety is limited — `f_read`, `f_write`, `f_lseek`, `f_close` are safe with re-entrancy enabled (`FF_FS_REENTRANT = 1` in `ffconf.h`), but directory operations like `f_opendir`, `f_readdir` may need additional locking.

### 2. SD Card Latency Spikes

SD cards are not real-time devices. Writes can stall for **50–250 ms** during internal garbage collection (wear leveling). Reads are more predictable but can still spike to **5–10 ms** for a single sector. The read-ahead cache in PSRAM absorbs these spikes.

### 3. Clock Speed

_core overclocks the RP2040 to 200–225 MHz. The RP2350 at 150 MHz is still faster per-clock (Cortex-M33 vs M0+, hardware multiply, etc.), but the DSP chain with 16 voices and 16 effects is computationally heavy. Profiling needed — overclocking the RP2350 to 200 MHz is possible if needed.

### 4. Fixed-Point Compatibility

_core uses Q16.16 fixed-point arithmetic throughout. This is pure C integer math — no architecture-specific code. It will compile and run identically on RP2350's Cortex-M33. The M33's single-cycle 32×32→64 multiply will actually make Q16.16 operations faster than on the M0+.

### 5. P4 Latency

Audio goes through the ESP32-P4, adding ~0.7 ms of latency (32-sample I2S buffer at 44100 Hz). For a sample playback instrument, this is negligible. For real-time monitoring of external audio (if _core ever adds audio input), it would be noticeable but still acceptable.

---

## File Structure for the Port

```
src/
  main.cpp                  ← entry point, core dispatch
  hw_config.c               ← SDIO configuration (see above)
  tbd16_audio.h / .c        ← audio transport driver (from TBD16_AUDIO_DRIVER.md)
  hw_input.h / .c           ← I2C UI abstraction
  pio_spi_oled.c / .h       ← OLED driver (existing, reuse as-is)
  psram_init.c              ← PSRAM init (existing, reuse as-is)

lib/
  _core/                    ← _core source (submodule or copy)
    lib/
      audio_callback.h      ← modify to use tbd16_audio + hw_input
      crossfade.h           ← unchanged (pure DSP)
      delay.h               ← unchanged, but use PSRAM for buffers
      freeverb.h            ← unchanged
      ...
  no-OS-FatFS-SD-SDIO-SPI-RPi-Pico/  ← existing, switch to SDIO mode
```

---

## Summary

| Aspect | Difficulty | Notes |
|--------|-----------|-------|
| Audio output | Easy | Drop-in tbd16_audio driver, add `>>3` |
| SD card (SDIO) | Easy | Hardware wiring is perfect, library supports it |
| Button/knob input | Medium | Need I2C abstraction layer for STM32 UI board |
| OLED display | Easy | Same resolution, just swap I2C for PIO SPI calls |
| MIDI | Easy | Hardware is there, more ports than zeptocore |
| DSP chain | None | Pure C integer math, compiles as-is |
| Multicore | Easy | Same Core 0/1 split, natural mapping |
| Read-ahead cache | Medium | Needed for glitch-free multi-voice playback |

The hardest part is not the hardware port — it's the software architecture change from direct GPIO/ADC reads to I2C-based input, and building a robust read-ahead cache to decouple SD latency from audio timing. The audio and SD card hardware mapping is essentially plug-and-play.
