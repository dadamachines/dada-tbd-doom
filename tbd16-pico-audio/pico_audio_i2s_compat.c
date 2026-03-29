// pico_audio_i2s_compat.c — pico_audio_i2s API routed through TBD-16 audio driver
//
// Provides audio_new_producer_pool(), take_audio_buffer(), give_audio_buffer(),
// audio_i2s_setup(), audio_i2s_connect(), audio_i2s_set_enabled() — the same
// API that pico-extras' pico_audio_i2s exposes.
//
// For projects like zeptocore that use the standard pico_audio_i2s API,
// this lets them compile and run on TBD-16 without audio code changes.

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "pico/audio_i2s.h"
#include "tbd16_audio.h"

// ── Static state ───────────────────────────────────────────────────────
static audio_buffer_pool_t pool_storage;
static audio_format_t      format_storage;
static uint32_t            stored_sample_rate = 44100;

// ── audio_new_producer_pool ────────────────────────────────────────────
audio_buffer_pool_t *audio_new_producer_pool(
    const audio_buffer_format_t *producer_format,
    uint32_t buffer_count,
    uint32_t samples_per_buffer)
{
    if (buffer_count > AUDIO_I2S_POOL_MAX_BUFFERS)
        buffer_count = AUDIO_I2S_POOL_MAX_BUFFERS;

    format_storage = *producer_format->format;
    pool_storage.format = &format_storage;
    pool_storage.buffer_count = buffer_count;
    pool_storage.samples_per_buffer = samples_per_buffer;

    uint32_t stride = producer_format->sample_stride;
    if (stride == 0) stride = 4;  // default stereo int16
    uint32_t bytes_per_buffer = samples_per_buffer * stride;
    uint32_t total = bytes_per_buffer * buffer_count;

    pool_storage.sample_data = (uint8_t *)malloc(total);
    if (!pool_storage.sample_data) {
        printf("[tbd16_compat] ERROR: malloc(%lu) failed\n", (unsigned long)total);
        return &pool_storage;
    }
    memset(pool_storage.sample_data, 0, total);

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
    (void)config;  // pin/PIO/DMA fields ignored on TBD-16
    if (intended_audio_format && intended_audio_format->sample_freq > 0)
        stored_sample_rate = intended_audio_format->sample_freq;
    return actual_audio_format;
}

// ── audio_i2s_connect ──────────────────────────────────────────────────
bool audio_i2s_connect(audio_buffer_pool_t *pool) {
    (void)pool;
    tbd16_audio_init(stored_sample_rate);
    return true;
}

// ── audio_i2s_set_enabled ──────────────────────────────────────────────
void audio_i2s_set_enabled(bool enabled) {
    (void)enabled;  // output is always active once connected
}

// ── take_audio_buffer ──────────────────────────────────────────────────
audio_buffer_t *take_audio_buffer(audio_buffer_pool_t *pool, bool block) {
    while (1) {
        // Backpressure: don't hand out buffers if the ring buffer
        // can't absorb another full buffer of samples.
        uint32_t buffered = tbd16_audio_buffered();
        uint32_t ring_free = (TBD16_RING_SIZE - 1) > buffered
                           ? (TBD16_RING_SIZE - 1) - buffered : 0;

        if (ring_free >= pool->samples_per_buffer) {
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

    // Copy samples into the TBD-16 ring buffer → SPI → P4 codec
    tbd16_audio_write((const int16_t *)buffer->buffer->bytes, buffer->sample_count);

    // Release the buffer back to the pool
    for (uint32_t i = 0; i < pool->buffer_count; i++) {
        if (&pool->buffers[i] == buffer) {
            pool->in_use[i] = 0;
            break;
        }
    }
}
