// PicoAudioBridge — RP2350 → P4 audio transport
// Provides a ring buffer, sample-rate converter, and SPI PCM packer
// so any RP2350 application can stream audio through the P4 codec.

#ifndef PICO_AUDIO_BRIDGE_H
#define PICO_AUDIO_BRIDGE_H

#include <stdint.h>
#include <stdbool.h>
#include "pico/audio_i2s.h"  // audio_buffer_t, audio_buffer_mem_t

#ifdef __cplusplus
extern "C" {
#endif

// PCM transport magic — must match PicoAudioBridge plugin on P4
#define PAB_MAGIC            0x50434D21  // "PCM!"
#define PAB_SAMPLES_PER_FRAME 32         // Stereo pairs per SPI frame (BUF_SZ)

// Ring buffer capacity in stereo sample pairs (power of 2)
#define PAB_RING_SIZE 2048
#define PAB_RING_MASK (PAB_RING_SIZE - 1)

// Source sample rate (set by the application, e.g. 49716 for Doom OPL)
#define PAB_SOURCE_FREQ 49716
// Target sample rate (P4 codec I2S)
#define PAB_TARGET_FREQ 44100

// Initialize the ring buffer. Call once at boot.
void pab_init(void);

// Get a temporary buffer for the mixer to fill.
// Returns NULL if the ring buffer is too full (backpressure).
audio_buffer_t *pab_take_buffer(void);

// Commit the filled buffer into the ring buffer.
void pab_give_buffer(audio_buffer_t *buf);

// Read PAB_SAMPLES_PER_FRAME stereo pairs from the ring buffer,
// resampling PAB_SOURCE_FREQ → PAB_TARGET_FREQ, and pack them into
// the given synth_midi[] region.
// Returns the number of bytes written (header + PCM), or 0 on error.
uint32_t pab_pack_spi(uint8_t *synth_midi_buf, uint32_t buf_size);

// Number of stereo sample pairs currently in the ring buffer.
uint32_t pab_available(void);

// Enable/disable 440 Hz sine test tone.
// When enabled, pab_pack_spi() generates a pure sine wave directly,
// bypassing the ring buffer and resampler entirely.
// Use this to isolate whether distortion is in the SPI/P4 chain
// or in the audio pipeline (ring buffer / resampler).
void pab_set_test_tone(bool enable);

#ifdef __cplusplus
}
#endif

#endif // PICO_AUDIO_BRIDGE_H
