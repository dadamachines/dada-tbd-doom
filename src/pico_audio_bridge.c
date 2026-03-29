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

// ── Resampler removed — P4 handles resampling via cubic Hermite ────────

// ── Test tone generator (440 Hz sine, bypasses ring buf) ──────────────
static bool test_tone_enabled = false;
static uint32_t test_tone_phase = 0;

// Source sample rate for PCM2 frames (default: Doom's OPL rate)
static uint16_t pab_source_rate = PAB_SOURCE_FREQ;

void pab_set_source_rate(uint16_t rate) {
    pab_source_rate = rate;
}

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
    if (n > PAB_RING_SIZE - 1) n = PAB_RING_SIZE - 1;

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
// (Kept for test tone which still outputs at 44100 Hz with 32 samples)
static uint32_t test_tone_input_needed(uint32_t out_count) {
    return out_count;  // test tone is generated directly, no resampling
}

uint32_t pab_pack_spi(uint8_t *synth_midi_buf, uint32_t buf_size) {
    // PCM2 header: 4 (magic) + 2 (count) + 2 (source_rate_hz) + samples
    // Max payload: 8 + 62*4 = 256 bytes (fits in synth_midi[256])

    // ── Test tone mode: 440 Hz sine, bypasses ring buffer ──
    if (test_tone_enabled) {
        uint32_t count_out = 32;  // test tone at 44100 Hz, 32 samples
        uint32_t payload_size = 8 + count_out * 4;
        if (buf_size < payload_size) return 0;

        uint32_t magic = PAB_MAGIC;
        uint16_t count16 = (uint16_t)count_out;
        uint16_t rate16 = 44100;  // test tone is already at 44100 Hz
        memcpy(synth_midi_buf, &magic, 4);
        memcpy(synth_midi_buf + 4, &count16, 2);
        memcpy(synth_midi_buf + 6, &rate16, 2);

        int16_t *out = (int16_t *)(synth_midi_buf + 8);
        for (uint32_t i = 0; i < count_out; i++) {
            int16_t val = sine256((uint8_t)(test_tone_phase >> 24));
            out[i * 2]     = val;
            out[i * 2 + 1] = val;
            test_tone_phase += TEST_TONE_PHASE_STEP;
        }
        return payload_size;
    }

    // ── Normal mode: pack raw samples, no resampling ──
    // Determine the source rate for this frame
#if AUDIO_TEST_TONES
    uint16_t rate16 = 44100;
#else
    uint16_t rate16 = pab_source_rate;
#endif

    // Cap samples per frame to match P4 codec consumption rate.
    // The P4 codec outputs 32 stereo pairs per I2S cycle at 44100 Hz,
    // so it processes one SPI frame per cycle (44100/32 = 1378.125 Hz).
    // Sending more samples than the P4 consumes per cycle overflows
    // the P4 ring buffer, causing it to drop oldest samples and
    // distort the audio (e.g. 1000 Hz appears as ~559 Hz).
    // Target: source_rate * 32 / 44100 samples per frame.
    uint32_t target_per_frame = ((uint32_t)rate16 * 32u) / 44100u;
    if (target_per_frame < 1) target_per_frame = 1;
    if (target_per_frame > PAB_SAMPLES_PER_FRAME) target_per_frame = PAB_SAMPLES_PER_FRAME;

    uint32_t avail = ring_count();
    uint32_t count = avail < target_per_frame ? avail : target_per_frame;

    if (count == 0) {
        // Ring buffer empty — track underruns for diagnostics
        static uint32_t underrun_count = 0;
        static uint32_t underrun_report = 0;
        underrun_count++;
        underrun_report++;
        if (underrun_report >= 5000) {  // every ~1 second at 5000 Hz ISR
            printf("[PAB] underruns=%lu (ring empty)\n", (unsigned long)underrun_count);
            underrun_report = 0;
        }
        return 0;
    }

    uint32_t payload_size = 8 + count * 4;
    if (buf_size < payload_size) return 0;

    // Copy raw samples from ring buffer (no resampling)
    int16_t *out = (int16_t *)(synth_midi_buf + 8);
    uint32_t r = ring_read;
    for (uint32_t i = 0; i < count; i++) {
        uint32_t idx = r & PAB_RING_MASK;
        out[i * 2]     = ring[idx * 2];
        out[i * 2 + 1] = ring[idx * 2 + 1];
        r++;
    }
    ring_read = r & PAB_RING_MASK;

    // Write PCM2 header
    uint32_t magic = PAB_MAGIC;
    uint16_t count16 = (uint16_t)count;
    memcpy(synth_midi_buf, &magic, 4);
    memcpy(synth_midi_buf + 4, &count16, 2);
    memcpy(synth_midi_buf + 6, &rate16, 2);

    return payload_size;
}
