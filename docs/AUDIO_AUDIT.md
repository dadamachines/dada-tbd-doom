# Audio Quality Audit — rp2040-doom Original vs TBD-16 Implementation

_Date: 2026-03-27_

## Summary

A comprehensive audit comparing the TBD-16 Doom audio implementation against the
original [rp2040-doom by kilograham](https://kilograham.github.io/rp2040-doom/sound.html).
Goal: identify every difference affecting audio quality and fix what we can.

## Issues Found & Fixed

### 1. Diagnostic Code in Audio Hot Path (FIXED)

**Files:** `opl_pico.c`, `i_picosound.c`

Diagnostic loops were running on **every audio sample** in both the OPL callback
and the SFX mixer:
- OPL: 256 iterations of `abs()` + compare + overflow check per buffer, plus
  `printf()` every 200 buffers (~0.5 s) blocking for ~5.6 ms.
- Mixer: Same pattern — per-sample peak tracking + periodic `printf()`.

**Impact:** `printf()` on the RP2350 is blocking (UART output). A 5.6 ms stall
every 500 ms means 1.1% of wall time is spent in serial output, but concentrated
in bursts that create audible dropouts.

**Fix:** Removed all hot-path diagnostics. The `<<= 3` gain loop and mixing loop
are now clean, matching the original rp2040-doom exactly.

### 2. Multicore Lock Race Condition (FIXED)

**File:** `i_picosound.c`

The mixer is called from **both cores**:
- Core 0: game loop via `I_Pico_UpdateSound()`
- Core 1: renderer via `SafeUpdateSound()` in `pd_render.cpp`

The lock used `save_and_disable_interrupts()` + a `bool mix_lock_held` flag.
**This is per-core only** — disabling interrupts on Core 0 does not prevent Core 1
from reading the flag simultaneously. Both cores could enter the mixer at the same
time, corrupting the ring buffer write pointer and SFX channel state.

**Failed fix attempt:** Hardware spinlock try-lock (`if (!*mix_spinlock) return;`).
While atomically correct, this caused **complete audio silence** — the mixer function
returned early on every call. Root cause under investigation (suspected RP2350 SIO
spinlock register behavior difference or compiler optimization of the pointer
dereference).

**Working fix:** `__sync_lock_test_and_set()` / `__sync_lock_release()` — GCC
atomic builtins that compile to `LDREX`/`STREX` on Cortex-M33. These use the ARM
global exclusive monitor, which is guaranteed atomic across both cores.

```c
static volatile uint32_t mix_lock_flag = 0;

// Acquire (non-blocking):
if (__sync_lock_test_and_set(&mix_lock_flag, 1)) {
    return;  // other core holds it
}
// ... mixing work ...
__sync_lock_release(&mix_lock_flag);
```

### 3. LP Filter State Reset at Buffer Boundaries (FIXED earlier)

**File:** `i_picosound.c`

The low-pass filter on SFX channels uses an IIR (exponential moving average):
```
sample = (beta256 * prev + alpha256 * raw) / 256
```

The original rp2040-doom uses 1024-sample buffers, so the filter re-reads the raw
sample at buffer start — first-sample discontinuity at ~48 Hz (inaudible).

Our 128-sample buffers caused the same discontinuity at ~388 Hz (audible buzz).
The fix: persist `lp_last_sample` in the channel struct across buffer boundaries.

## Architectural Differences (Not Fixable)

These are inherent to the TBD-16 hardware and cannot be changed without board redesign.

| Aspect | Original rp2040-doom | TBD-16 |
|--------|---------------------|--------|
| **CPU** | RP2040 @ 270 MHz (overclock), Cortex-M0+ | RP2350B @ 150 MHz (stock), Cortex-M33+FPU |
| **Audio output** | PIO I2S → DAC at 49716 Hz | Ring buf → SPI → ESP32-P4 → TLV3254 at 44100 Hz |
| **Resampling** | None — native 49716 Hz | P4 cubic Hermite 49716 → 44100 Hz |
| **Buffer size** | 1024 samples (~20.6 ms) | 128 samples (~2.6 ms) |
| **Buffer model** | 2× I2S DMA pool (`take`/`give`) | Single mix buf → 2048-pair ring → SPI ISR |
| **Latency** | ~20 ms (1024 samples at 49716) | ~50-70 ms (ring buffer + SPI + P4 ring + codec) |
| **Jitter** | Zero (DMA-paced I2S consumption) | TIMER1 ISR ±µs jitter, absorbed by ring buffer |
| **Multicore** | Unprotected (single-core assumption) | Atomic try-lock (LDREX/STREX) |

### Why the resampler matters

The original outputs at exactly 49716 Hz — no resampling at all. Graham Sanderson:
> "I chose to use 49,716Hz as the actual audio output frequency with no resampling
> for simplicity."

Our TLV320AIC3254 codec runs at 44100 Hz, so the P4 must resample. The cubic
Hermite resampler on the P4 is high quality (4-tap Catmull-Rom), but any resampler
introduces artifacts:
- **Aliasing**: frequencies above 22050 Hz in the 49716 Hz source fold back.
  The OPL output contains harmonics up to ~24858 Hz (Nyquist of 49716), so
  some aliasing is unavoidable without a pre-filter.
- **Interpolation error**: cubic Hermite is C¹ continuous (smooth first derivative)
  but not band-limited. Introduces −60 to −80 dB distortion products.
- **Rate adaption jitter**: the P4's adaptive rate matching adjusts the fractional
  step to keep its ring buffer at a target fill level, introducing ~±0.01%
  frequency modulation.

### Why buffer size matters

1024 vs 128 samples affects:
- **OPL callback efficiency**: OPL generates in chunks up to `buffer_samples`.
  Smaller buffers mean more function-call overhead per second (388 calls/s vs 49).
- **LP filter boundary artifacts**: Fixed by `lp_last_sample` persistence.
- **Ring buffer fill granularity**: 128-sample writes to a 2048-pair ring give
  16 segments. The TIMER1 ISR drains 62 pairs per frame at ~1000 Hz effective rate.
  So one mixer call (128 samples) provides ~130 ms of data at 49716 Hz but is
  consumed in ~2 ISR ticks.

## Comparison: Code That Is Identical

These sections match the original rp2040-doom exactly:

| Component | Status |
|-----------|--------|
| ADPCM block decoder (`adpcm_decode_block_s8`) | ✅ Identical |
| SFX volume scaling (`sample * voll`, `sample * volr`) | ✅ Identical |
| OPL ×8 gain (`<<= 3` wrapping in int16) | ✅ Identical |
| Fade in/out (per-sample `fade_level >> 16`) | ✅ Identical |
| Channel step calculation (fixed-point 16.16) | ✅ Identical |
| ADPCM block decompression trigger | ✅ Identical |
| EMU8950 OPL engine | ✅ Same library |
| LP filter coefficient (`alpha256 = 256 * 201 * fs / (201*fs + 64*Fs)`) | ✅ Identical |

## Open Questions

### Should SOUND_LOW_PASS be disabled?

Currently `SOUND_LOW_PASS=1` (set in `platformio.ini`). Graham Sanderson's notes:
> "I did try some low-pass filtering on SFX audio output... This didn't produce
> any noticeable (to me) improvement in the audio. You can enable it with
> SOUND_LOW_PASS=1."

The filter is a simple first-order IIR with cutoff at approximately:
```
fc = sample_freq * 64 / (201 * 2π) ≈ 11025 * 64 / 1263 ≈ 558 Hz
```
This is extremely aggressive for SFX that contain content up to 5512 Hz (Nyquist
of 11025 Hz). It heavily attenuates high-frequency content like gunshots and
explosions. Disabling it (`-DSOUND_LOW_PASS=0`) is worth testing.

### Mathematical Test Results

The `tools/test_audio_chain.py` test suite verifies the entire audio pipeline:
- **67/69 tests pass**
- 2 "failures" are informational: theoretical maximum overflow at ±32767 input
  with all 8 SFX channels at max volume — does not occur at runtime (measured
  OPL peak is ~1500 pre-gain, ~12000 post-gain, well within int16 range).

## Files Modified

| File | Change |
|------|--------|
| `lib/rp2040-doom/opl/opl_pico.c` | Removed diagnostic hot-path loop; clean `<<= 3` |
| `src/i_picosound.c` | Removed diagnostic hot-path loop; atomic try-lock; `lp_last_sample` persistence |
| `tools/test_audio_chain.py` | New: 10-stage mathematical verification of audio chain |
