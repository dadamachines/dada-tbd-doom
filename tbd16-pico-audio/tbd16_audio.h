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

// Ring buffer capacity in stereo sample pairs (power of 2).
// Exposed so the pico_audio_i2s compatibility layer can do backpressure.
#define TBD16_RING_SIZE 2048

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
