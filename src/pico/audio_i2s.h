// Stub pico/audio_i2s.h for TBD-16 (no pico-extras, sound via P4)
// Provides minimal type definitions so doom audio code compiles.
#pragma once

#include "pico.h"

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
    uint16_t format;
    uint16_t channel_count;
} audio_format_t;

typedef struct audio_buffer_pool {
    audio_format_t *format;
} audio_buffer_pool_t;
