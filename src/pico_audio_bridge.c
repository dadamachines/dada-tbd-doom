// PicoAudioBridge — ring buffer, resampler, SPI PCM packer
// See pico_audio_bridge.h for API.

#include <stdio.h>          // MUST be first — avoids PSRAM init hang on RP2350
#include <string.h>
#include "pico_audio_bridge.h"

// ── Ring buffer (int16 stereo, interleaved L R L R …) ──────────────────
static int16_t ring[PAB_RING_SIZE * 2];           // *2 for stereo
static volatile uint32_t ring_write = 0;          // written by mixer
static volatile uint32_t ring_read  = 0;          // read by SPI packer

// ── Temporary mix buffer ───────────────────────────────────────────────
#define MIX_BUF_SAMPLES 128
static int16_t mix_pcm[MIX_BUF_SAMPLES * 2];     // stereo int16
static audio_buffer_mem_t mix_mem;
static audio_buffer_t mix_buf;

// ── Resampler state (49716 → 44100 fixed-point) ───────────────────────
// step = PAB_SOURCE_FREQ / PAB_TARGET_FREQ in 16.16 fixed point
#define RESAMPLE_STEP ((uint32_t)((uint64_t)PAB_SOURCE_FREQ * 65536 / PAB_TARGET_FREQ))
static uint32_t resample_frac = 0;  // fractional accumulator (0..65535)

// ── Test tone generator (440 Hz sine, bypasses ring buf + resampler) ──
static bool test_tone_enabled = false;
static uint32_t test_tone_phase = 0;

// Phase step for 440 Hz at 44100 Hz with 32-bit accumulator, 256-entry table:
// step = 440 * 2^32 / 44100 = 42852277
#define TEST_TONE_PHASE_STEP 42852277u

// Quarter-wave sine table (64 entries, amplitude 20000)
static const int16_t sine_qw[64] = {
        0,   499,   997,  1495,  1991,  2487,  2981,  3473,
     3963,  4450,  4935,  5417,  5895,  6370,  6840,  7307,
     7769,  8226,  8678,  9124,  9565, 10000, 10429, 10851,
    11266, 11675, 12076, 12470, 12856, 13234, 13603, 13965,
    14317, 14661, 14996, 15321, 15637, 15943, 16239, 16525,
    16801, 17066, 17321, 17564, 17797, 18019, 18230, 18430,
    18617, 18794, 18959, 19111, 19252, 19382, 19499, 19603,
    19696, 19777, 19845, 19901, 19944, 19975, 19994, 20000,
};

// Lookup full 256-point sine from quarter-wave table
static inline int16_t sine256(uint8_t idx) {
    if (idx < 64)  return  sine_qw[idx];
    if (idx < 128) return  sine_qw[127 - idx];
    if (idx < 192) return -sine_qw[idx - 128];
    return                 -sine_qw[255 - idx];
}

void pab_set_test_tone(bool enable) {
    test_tone_enabled = enable;
    test_tone_phase = 0;
    printf("[PAB] Test tone %s\n", enable ? "ENABLED (440 Hz sine)" : "disabled");
}

static inline uint32_t ring_count(void) {
    return (ring_write - ring_read) & PAB_RING_MASK;
}

static inline uint32_t ring_free(void) {
    return (PAB_RING_SIZE - 1) - ring_count();
}

void pab_init(void) {
    ring_write = 0;
    ring_read  = 0;
    resample_frac = 0;
    memset(ring, 0, sizeof(ring));

    mix_mem.bytes = (uint8_t *)mix_pcm;
    mix_mem.size  = sizeof(mix_pcm);

    mix_buf.buffer           = &mix_mem;
    mix_buf.max_sample_count = MIX_BUF_SAMPLES;
    mix_buf.sample_count     = 0;
}

uint32_t pab_available(void) {
    return ring_count();
}

audio_buffer_t *pab_take_buffer(void) {
    // Only hand out a buffer if there's enough room to absorb it
    if (ring_free() < MIX_BUF_SAMPLES)
        return NULL;

    memset(mix_pcm, 0, sizeof(mix_pcm));
    mix_buf.sample_count = 0;
    return &mix_buf;
}

void pab_give_buffer(audio_buffer_t *buf) {
    if (!buf || buf->sample_count == 0) return;

    int16_t *src = (int16_t *)buf->buffer->bytes;
    uint32_t n = buf->sample_count;
    if (n > MIX_BUF_SAMPLES) n = MIX_BUF_SAMPLES;

    uint32_t w = ring_write;
    for (uint32_t i = 0; i < n; i++) {
        uint32_t idx = (w + i) & PAB_RING_MASK;
        ring[idx * 2]     = src[i * 2];       // L
        ring[idx * 2 + 1] = src[i * 2 + 1];   // R
    }
    ring_write = (w + n) & PAB_RING_MASK;
}

// Number of input samples (49716 Hz) needed to produce `out_count`
// output samples (44100 Hz) with the current fractional state.
static uint32_t input_needed(uint32_t out_count) {
    // Each output sample advances the read pointer by RESAMPLE_STEP
    // in 16.16 fixed point. We need the integer part sum + 1 for
    // interpolation lookahead.
    uint64_t total_frac = (uint64_t)resample_frac + (uint64_t)RESAMPLE_STEP * out_count;
    return (uint32_t)(total_frac >> 16) + 1;  // +1 for interp lookahead
}

uint32_t pab_pack_spi(uint8_t *synth_midi_buf, uint32_t buf_size) {
    // Header: 4 (magic) + 4 (count) + PAB_SAMPLES_PER_FRAME*4 (stereo int16)
    uint32_t payload_size = 8 + PAB_SAMPLES_PER_FRAME * 4;
    if (buf_size < payload_size) return 0;

    // ── Test tone mode: 440 Hz sine, bypasses ring buffer + resampler ──
    if (test_tone_enabled) {
        uint32_t magic = PAB_MAGIC;
        uint32_t count = PAB_SAMPLES_PER_FRAME;
        memcpy(synth_midi_buf, &magic, 4);
        memcpy(synth_midi_buf + 4, &count, 4);

        int16_t *out = (int16_t *)(synth_midi_buf + 8);
        for (uint32_t i = 0; i < PAB_SAMPLES_PER_FRAME; i++) {
            int16_t val = sine256((uint8_t)(test_tone_phase >> 24));
            out[i * 2]     = val;  // L
            out[i * 2 + 1] = val;  // R
            test_tone_phase += TEST_TONE_PHASE_STEP;
        }
        return payload_size;
    }

    uint32_t avail = ring_count();
    uint32_t needed = input_needed(PAB_SAMPLES_PER_FRAME);
    if (avail < needed) {
        // Not enough samples — output silence frame
        uint32_t magic = PAB_MAGIC;
        uint32_t count = PAB_SAMPLES_PER_FRAME;
        memcpy(synth_midi_buf, &magic, 4);
        memcpy(synth_midi_buf + 4, &count, 4);
        memset(synth_midi_buf + 8, 0, PAB_SAMPLES_PER_FRAME * 4);
        return payload_size;
    }

    // Resample with linear interpolation
    int16_t *out = (int16_t *)(synth_midi_buf + 8);
    uint32_t r = ring_read;
    uint32_t frac = resample_frac;

    for (uint32_t i = 0; i < PAB_SAMPLES_PER_FRAME; i++) {
        uint32_t idx0 = r & PAB_RING_MASK;
        uint32_t idx1 = (r + 1) & PAB_RING_MASK;
        uint32_t f = frac >> 8;  // 8-bit fraction for lerp (0..255)

        // Left channel
        int32_t l0 = ring[idx0 * 2];
        int32_t l1 = ring[idx1 * 2];
        out[i * 2] = (int16_t)(l0 + ((l1 - l0) * (int32_t)f >> 8));

        // Right channel
        int32_t r0 = ring[idx0 * 2 + 1];
        int32_t r1 = ring[idx1 * 2 + 1];
        out[i * 2 + 1] = (int16_t)(r0 + ((r1 - r0) * (int32_t)f >> 8));

        // Advance fractional position
        frac += RESAMPLE_STEP;
        r += (frac >> 16);
        frac &= 0xFFFF;
    }

    ring_read = r & PAB_RING_MASK;
    resample_frac = frac;

    // Write header
    uint32_t magic = PAB_MAGIC;
    uint32_t count = PAB_SAMPLES_PER_FRAME;
    memcpy(synth_midi_buf, &magic, 4);
    memcpy(synth_midi_buf + 4, &count, 4);

    return payload_size;
}
