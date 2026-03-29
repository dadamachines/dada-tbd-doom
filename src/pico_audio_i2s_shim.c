// pico_audio_i2s_shim.c — Drop-in pico_audio_i2s implementation for TBD-16
//
// Provides audio_new_producer_pool(), take_audio_buffer(), give_audio_buffer(),
// audio_i2s_setup(), audio_i2s_connect(), audio_i2s_set_enabled() — the same
// API that pico-extras' pico_audio_i2s exposes.  Internally, samples are
// routed through the PicoAudioBridge ring buffer → SPI → ESP32-P4 codec.
//
// This allows projects like _core (zeptocore) to compile and run on TBD-16
// without any audio code changes.

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "pico/audio_i2s.h"
#include "pico_audio_bridge.h"

// ── Static state ───────────────────────────────────────────────────────
static audio_buffer_pool_t pool_storage;
static audio_format_t      format_storage;
static bool                shim_enabled = false;

// ── audio_new_producer_pool ────────────────────────────────────────────
audio_buffer_pool_t *audio_new_producer_pool(
    const audio_buffer_format_t *producer_format,
    uint32_t buffer_count,
    uint32_t samples_per_buffer)
{
    if (buffer_count > AUDIO_I2S_POOL_MAX_BUFFERS)
        buffer_count = AUDIO_I2S_POOL_MAX_BUFFERS;

    // Copy the audio format so the pool owns it
    format_storage = *producer_format->format;
    pool_storage.format = &format_storage;
    pool_storage.buffer_count = buffer_count;
    pool_storage.samples_per_buffer = samples_per_buffer;

    // Allocate backing memory for all buffers (stereo int16 = 4 bytes/frame)
    uint32_t stride = producer_format->sample_stride;
    if (stride == 0) stride = 4;  // default stereo int16
    uint32_t bytes_per_buffer = samples_per_buffer * stride;
    uint32_t total = bytes_per_buffer * buffer_count;

    pool_storage.sample_data = (uint8_t *)malloc(total);
    if (!pool_storage.sample_data) {
        printf("[audio_i2s_shim] ERROR: malloc(%lu) failed\n", (unsigned long)total);
        return &pool_storage;
    }
    memset(pool_storage.sample_data, 0, total);

    // Wire up each buffer
    for (uint32_t i = 0; i < buffer_count; i++) {
        pool_storage.mems[i].bytes = pool_storage.sample_data + i * bytes_per_buffer;
        pool_storage.mems[i].size  = bytes_per_buffer;
        pool_storage.buffers[i].buffer           = &pool_storage.mems[i];
        pool_storage.buffers[i].max_sample_count = samples_per_buffer;
        pool_storage.buffers[i].sample_count     = 0;
        pool_storage.in_use[i] = 0;
    }

    return &pool_storage;
}

// ── audio_i2s_setup ────────────────────────────────────────────────────
const audio_format_t *audio_i2s_setup(
    const audio_format_t *intended_audio_format,
    const audio_format_t *actual_audio_format,
    const audio_i2s_config_t *config)
{
    (void)config;  // pin/PIO/DMA fields are ignored on TBD-16

    // Tell the PAB transport what sample rate the producer is running at
    // so the P4 resampler gets the correct rate in PCM2 frame headers.
    if (intended_audio_format && intended_audio_format->sample_freq > 0) {
        pab_set_source_rate((uint16_t)intended_audio_format->sample_freq);
    }

    return actual_audio_format;
}

// ── audio_i2s_connect ──────────────────────────────────────────────────
bool audio_i2s_connect(audio_buffer_pool_t *pool) {
    (void)pool;
    pab_init();
    return true;
}

// ── audio_i2s_set_enabled ──────────────────────────────────────────────
void audio_i2s_set_enabled(bool enabled) {
    shim_enabled = enabled;
}

// ── take_audio_buffer ──────────────────────────────────────────────────
audio_buffer_t *take_audio_buffer(audio_buffer_pool_t *pool, bool block) {
    while (1) {
        // Backpressure: don't hand out buffers if the PAB ring buffer
        // can't absorb another full buffer of samples.
        uint32_t ring_used = pab_available();
        uint32_t ring_free = (PAB_RING_SIZE - 1) > ring_used
                           ? (PAB_RING_SIZE - 1) - ring_used : 0;

        if (ring_free >= pool->samples_per_buffer) {
            // Find a free buffer in the pool
            for (uint32_t i = 0; i < pool->buffer_count; i++) {
                if (!pool->in_use[i]) {
                    pool->in_use[i] = 1;
                    pool->buffers[i].sample_count = 0;
                    return &pool->buffers[i];
                }
            }
        }

        if (!block) return NULL;
        tight_loop_contents();
    }
}

// ── give_audio_buffer ──────────────────────────────────────────────────
void give_audio_buffer(audio_buffer_pool_t *pool, audio_buffer_t *buffer) {
    if (!buffer || buffer->sample_count == 0) return;

    // Push samples into the PAB ring buffer (same path as Doom audio)
    pab_give_buffer(buffer);

    // Release the buffer back to the pool
    for (uint32_t i = 0; i < pool->buffer_count; i++) {
        if (&pool->buffers[i] == buffer) {
            pool->in_use[i] = 0;
            break;
        }
    }
}
