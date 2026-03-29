// pico/audio_i2s.h — Drop-in pico_audio_i2s API for TBD-16
//
// On standard Pico boards, pico_audio_i2s drives a DAC via I2S (PIO).
// On TBD-16, there is no direct DAC — audio goes through SPI to the
// ESP32-P4 codec.  This header provides the same types and functions
// so projects like _core (zeptocore) compile and run without changes.
//
// The implementation lives in pico_audio_i2s_compat.c and routes all
// audio through the TBD-16 audio driver → SPI → P4 codec.
#pragma once

#include "pico.h"
#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

// ── PCM format constants ───────────────────────────────────────────────
#define AUDIO_PCM_FORMAT_S8  0
#define AUDIO_PCM_FORMAT_S16 1
#define AUDIO_PCM_FORMAT_S32 2
#define AUDIO_BUFFER_FORMAT_PCM_S16 AUDIO_PCM_FORMAT_S16

// ── Core types ─────────────────────────────────────────────────────────

typedef struct audio_buffer_mem {
    uint8_t *bytes;
    uint32_t size;
} audio_buffer_mem_t;

typedef struct audio_buffer {
    audio_buffer_mem_t *buffer;
    uint32_t sample_count;
    uint32_t max_sample_count;
} audio_buffer_t;

typedef struct audio_format {
    uint32_t sample_freq;
    union {
        uint16_t pcm_format;   // _core uses this name
        uint16_t format;       // pico-extras uses this name
    };
    uint16_t channel_count;
} audio_format_t;

typedef struct audio_buffer_format {
    audio_format_t *format;
    uint16_t sample_stride;    // bytes per sample frame (e.g. 4 for stereo int16)
} audio_buffer_format_t;

typedef struct audio_i2s_config {
    uint8_t data_pin;          // ignored on TBD-16
    uint8_t clock_pin_base;    // ignored on TBD-16
    uint8_t dma_channel;       // ignored on TBD-16
    uint8_t pio_sm;            // ignored on TBD-16
} audio_i2s_config_t;

// ── Buffer pool ────────────────────────────────────────────────────────
// Holds N pre-allocated audio buffers that cycle between producer and
// the ring buffer consumer.

#define AUDIO_I2S_POOL_MAX_BUFFERS 4

typedef struct audio_buffer_pool {
    audio_format_t *format;
    uint32_t buffer_count;
    uint32_t samples_per_buffer;
    audio_buffer_t     buffers[AUDIO_I2S_POOL_MAX_BUFFERS];
    audio_buffer_mem_t mems[AUDIO_I2S_POOL_MAX_BUFFERS];
    uint8_t           *sample_data;       // backing allocation
    volatile uint8_t   in_use[AUDIO_I2S_POOL_MAX_BUFFERS];
} audio_buffer_pool_t;

// ── pico_audio_i2s–compatible API ──────────────────────────────────────

// Create a pool of `buffer_count` audio buffers, each holding
// `samples_per_buffer` stereo sample frames.
audio_buffer_pool_t *audio_new_producer_pool(
    const audio_buffer_format_t *producer_format,
    uint32_t buffer_count,
    uint32_t samples_per_buffer);

// Configure I2S output.  On TBD-16 the pin/PIO fields are ignored;
// the sample rate is forwarded to the audio transport.
const audio_format_t *audio_i2s_setup(
    const audio_format_t *intended_audio_format,
    const audio_format_t *actual_audio_format,
    const audio_i2s_config_t *config);

// Connect a producer pool to the output.  On TBD-16 this initialises
// the SPI audio transport and activates the P4 codec plugin.
bool audio_i2s_connect(audio_buffer_pool_t *pool);

// Enable or disable audio output.
void audio_i2s_set_enabled(bool enabled);

// Acquire a free buffer to fill with audio.
//   block=true  → spin until one is available
//   block=false → return NULL immediately if none free
audio_buffer_t *take_audio_buffer(audio_buffer_pool_t *pool, bool block);

// Submit a filled buffer.  Copies samples into the ring buffer
// for SPI transport and marks the buffer as free for reuse.
void give_audio_buffer(audio_buffer_pool_t *pool, audio_buffer_t *buffer);

#ifdef __cplusplus
}
#endif
