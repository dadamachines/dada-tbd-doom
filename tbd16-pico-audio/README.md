# TBD-16 Pico Audio

Drop-in audio driver for the RP2350 on DaDa Machines TBD-16 hardware. Routes audio through SPI to the ESP32-P4 codec (TLV320AIC3254).

Copy this folder into your project and get audio working.

## Quick Start

### Option A: Simple API

For custom projects, picoX7-style synths, or anything that generates samples directly.

```c
#include "tbd16_audio.h"

int main() {
    stdio_init_all();
    tbd16_audio_init(44100);

    while (true) {
        int16_t samples[64];  // stereo interleaved: L0 R0 L1 R1 ...
        generate_your_audio(samples, 32);  // 32 stereo pairs
        tbd16_audio_write(samples, 32);    // blocks if ring full
    }
}
```

### Option B: Callback API

For synth engines that generate audio in a callback (like picoX7's `getSamples()`).

```c
#include "tbd16_audio.h"

void my_callback(int16_t *stereo_buf, uint32_t num_pairs) {
    // Called from ISR at ~1378 Hz — keep it fast (~700 µs budget)
    for (uint32_t i = 0; i < num_pairs; i++) {
        int16_t sample = generate_next_sample();
        stereo_buf[i * 2]     = sample;  // L
        stereo_buf[i * 2 + 1] = sample;  // R
    }
}

int main() {
    stdio_init_all();
    tbd16_audio_init_with_callback(44100, my_callback);
    while (true) { /* handle MIDI, UI, etc. */ }
}
```

### Option C: pico_audio_i2s Compatible (zero code changes)

For projects like zeptocore that use the standard pico-extras `pico_audio_i2s` API. **No audio code changes needed.**

```c
// This code compiles and runs unmodified on TBD-16:
audio_format_t af = { .pcm_format = AUDIO_PCM_FORMAT_S16,
                      .sample_freq = 44100, .channel_count = 2 };
audio_buffer_format_t abf = { .format = &af, .sample_stride = 4 };
audio_buffer_pool_t *pool = audio_new_producer_pool(&abf, 3, SAMPLES_PER_BUFFER);
audio_i2s_setup(&af, &af, &config);
audio_i2s_connect(pool);
audio_i2s_set_enabled(true);

// Audio loop (unchanged)
audio_buffer_t *buf = take_audio_buffer(pool, true);
int16_t *samples = (int16_t *)buf->buffer->bytes;
// ... fill samples ...
buf->sample_count = buf->max_sample_count;
give_audio_buffer(pool, buf);
```

Just remove `pico_audio_i2s` from your link libraries and link `tbd16_audio` instead.

## Integration

### CMake (Pico SDK projects)

1. Copy `tbd16-pico-audio/` into your project
2. In your `CMakeLists.txt`:

```cmake
add_subdirectory(tbd16-pico-audio)
target_link_libraries(your_target tbd16_audio)   # includes pico_audio_i2s compat
```

If you only need the simple/callback API (no pico_audio_i2s compatibility):

```cmake
target_link_libraries(your_target tbd16_audio_core)
```

3. Remove `pico_audio_i2s` from any existing `target_link_libraries`

### PlatformIO

1. Copy `tbd16-pico-audio/` into `lib/`
2. Add to `platformio.ini`:

```ini
build_src_filter = +<../lib/tbd16-pico-audio/tbd16_audio.c>
                   +<../lib/tbd16-pico-audio/pico_audio_i2s_compat.c>
build_flags = -Ilib/tbd16-pico-audio
```

## Output Level

The P4 codec applies **×8 linear gain** to compensate for quiet sources like Doom. If your project outputs near full-scale int16 (±32767), you'll get clipping.

| Your Peak | After ×8 | Result |
|---|---|---|
| ±4096 (-18 dBFS) | ±32768 | Full scale, clean |
| ±16384 (-6 dBFS) | ±131072 | **Hard clipped** |
| ±32767 (0 dBFS) | ±262136 | **Severely clipped** |

**Fix**: Attenuate before output:

```c
sample >>= 3;  // ÷8 to compensate for P4 gain
```

Or reduce the gain in the PicoAudioBridge plugin config on the P4 side.

## Sample Rate

Your source rate can be anything — the P4 resamples automatically:

| Source Rate | Use Case | P4 Action |
|---|---|---|
| 44100 Hz | Standard audio, zeptocore | Pass-through |
| 48000 Hz | USB audio standard | Resample to 44100 |
| 49096 Hz | picoX7 | Resample to 44100 |
| 49716 Hz | Doom OPL | Resample to 44100 |

## What's In This Folder

| File | Purpose |
|---|---|
| `tbd16_audio.h` | Core API: init, write, callback, test tone |
| `tbd16_audio.c` | Self-contained driver: ring buffer, SPI, DMA, ISR, control link |
| `pico/audio_i2s.h` | pico_audio_i2s compatible types and function declarations |
| `pico_audio_i2s_compat.c` | Shim: routes pico_audio_i2s calls through tbd16_audio |
| `CMakeLists.txt` | CMake integration (two targets) |
| `HARDWARE.md` | Complete pin map, peripherals, resource conflicts |

## Hardware Reference

See [HARDWARE.md](HARDWARE.md) for the complete TBD-16 pin map — every GPIO assignment, SPI links, OLED, SD card, PSRAM, I2C buttons, MIDI ports, and resource conflicts to watch when porting.

## Architecture

For the full architecture, SPI protocol details, gotchas, and implementation notes, see [TBD16_AUDIO_DRIVER.md](../docs/TBD16_AUDIO_DRIVER.md).

```
Your Code ──▶ Ring Buffer ──▶ TIMER1 ISR ──▶ SPI1 DMA ──▶ ESP32-P4 ──▶ TLV320 DAC
             (2048 pairs)     (5000 Hz)      (25 MHz)      (resample)   (44100 Hz)
```
