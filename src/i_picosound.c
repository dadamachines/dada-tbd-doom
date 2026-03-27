// TBD-16 Doom — Sound module for PicoAudioBridge
// Based on rp2040-doom/src/pico/i_picosound.c but outputs via
// pico_audio_bridge ring buffer → SPI → P4 plugin instead of I2S.

#include <stdio.h>          // MUST be first — avoids PSRAM init hang on RP2350
#include "config.h"

#include <string.h>
#include <assert.h>
#include <doom/sounds.h>
#include <z_zone.h>

#include "deh_str.h"
#include "i_sound.h"
#include "m_misc.h"
#include "w_wad.h"
#include "doomtype.h"
#include "pico_audio_bridge.h"
//#include "p4_control_link.h"
#include "p4_spi_transport.h"
#include "picoflash.h"
#if AUDIO_TEST_TONES
#include <math.h>
#endif

// ── ADPCM decoder (same as original i_picosound.c) ────────────────────

#define ADPCM_BLOCK_SIZE 128
#define ADPCM_SAMPLES_PER_BLOCK_SIZE 249
#define MIX_MAX_VOLUME 128

#define PICO_SOUND_SAMPLE_FREQ PAB_SOURCE_FREQ

typedef struct channel_s channel_t;

static volatile enum {
    FS_NONE,
    FS_FADE_OUT,
    FS_FADE_IN,
    FS_SILENT,
} fade_state;
#define FADE_STEP 8
static uint16_t fade_level;

struct channel_s {
    const uint8_t *data;
    const uint8_t *data_end;
    uint32_t offset;
    uint32_t step;
    uint8_t left, right;
    uint8_t decompressed_size;
#if SOUND_LOW_PASS
    uint8_t alpha256;
    int     lp_last_sample;   // persisted low-pass filter accumulator
#endif
    int8_t decompressed[ADPCM_SAMPLES_PER_BLOCK_SIZE];
};

static void (*music_generator)(audio_buffer_t *buffer);
static boolean sound_initialized = false;
static channel_t channels[NUM_SOUND_CHANNELS];
static boolean use_sfx_prefix;

#define CLIP(data, min, max) \
if ((data) > (max)) data = max; \
else if ((data) < (min)) data = min;

static const uint16_t step_table[89] = {
    7, 8, 9, 10, 11, 12, 13, 14,
    16, 17, 19, 21, 23, 25, 28, 31,
    34, 37, 41, 45, 50, 55, 60, 66,
    73, 80, 88, 97, 107, 118, 130, 143,
    157, 173, 190, 209, 230, 253, 279, 307,
    337, 371, 408, 449, 494, 544, 598, 658,
    724, 796, 876, 963, 1060, 1166, 1282, 1411,
    1552, 1707, 1878, 2066, 2272, 2499, 2749, 3024,
    3327, 3660, 4026, 4428, 4871, 5358, 5894, 6484,
    7132, 7845, 8630, 9493, 10442, 11487, 12635, 13899,
    15289, 16818, 18500, 20350, 22385, 24623, 27086, 29794,
    32767
};

static const int index_table[] = {
    -1, -1, -1, -1, 2, 4, 6, 8
};

static inline bool is_channel_playing(int channel) {
    return channels[channel].decompressed_size != 0;
}

static inline void stop_channel(int channel) {
    channels[channel].decompressed_size = 0;
}

static bool check_and_init_channel(int channel) {
    return sound_initialized && ((unsigned)channel) < NUM_SOUND_CHANNELS;
}

static int adpcm_decode_block_s8(int8_t *outbuf, const uint8_t *inbuf, int inbufsize) {
    int samples = 1, chunks;
    if (inbufsize < 4) return 0;

    int32_t pcmdata = (int16_t)(inbuf[0] | (inbuf[1] << 8));
    *outbuf++ = pcmdata >> 8u;
    int index = inbuf[2];

    if (index < 0 || index > 88 || inbuf[3]) return 0;

    inbufsize -= 4;
    inbuf += 4;
    chunks = inbufsize / 4;
    samples += chunks * 8;

    while (chunks--) {
        for (int i = 0; i < 4; ++i) {
            int step = step_table[index], delta = step >> 3;
            if (*inbuf & 1) delta += (step >> 2);
            if (*inbuf & 2) delta += (step >> 1);
            if (*inbuf & 4) delta += step;
            if (*inbuf & 8) delta = -delta;
            pcmdata += delta;
            index += index_table[*inbuf & 0x7];
            CLIP(index, 0, 88);
            CLIP(pcmdata, -32768, 32767);
            outbuf[i * 2] = pcmdata >> 8u;

            step = step_table[index]; delta = step >> 3;
            if (*inbuf & 0x10) delta += (step >> 2);
            if (*inbuf & 0x20) delta += (step >> 1);
            if (*inbuf & 0x40) delta += step;
            if (*inbuf & 0x80) delta = -delta;
            pcmdata += delta;
            index += index_table[(*inbuf >> 4) & 0x7];
            CLIP(index, 0, 88);
            CLIP(pcmdata, -32768, 32767);
            outbuf[i * 2 + 1] = pcmdata >> 8u;
            inbuf++;
        }
        outbuf += 8;
    }
    return samples;
}

static void decompress_buffer(channel_t *channel) {
    if (channel->data == channel->data_end) {
        channel->decompressed_size = 0;
    } else {
        int block_size = channel->data_end - channel->data;
        if (block_size > ADPCM_BLOCK_SIZE) block_size = ADPCM_BLOCK_SIZE;
        channel->decompressed_size = adpcm_decode_block_s8(
            channel->decompressed, channel->data, block_size);
        assert(channel->decompressed_size &&
               channel->decompressed_size <= sizeof(channel->decompressed));
        channel->data += block_size;
    }
}

static boolean init_channel_for_sfx(channel_t *ch, const sfxinfo_t *sfxinfo, int pitch) {
    int lumpnum = sfx_mut(sfxinfo)->lumpnum;
    int lumplen = W_LumpLength(lumpnum);
    const uint8_t *data = W_CacheLumpNum(lumpnum, PU_STATIC);

    if (lumplen < 8 || data[0] != 0x03 || data[1] != 0x80) return false;

    int length = lumplen - 8;
    ch->data = data + 8;
    ch->data_end = ch->data + length;

    uint32_t sample_freq = (data[3] << 8) | data[2];
    if (pitch == NORM_PITCH)
        ch->step = sample_freq * 65536 / PICO_SOUND_SAMPLE_FREQ;
    else
        ch->step = (uint32_t)((uint64_t)sample_freq * pitch * 65536u /
                              ((uint64_t)PICO_SOUND_SAMPLE_FREQ * NORM_PITCH));

    decompress_buffer(ch);
    ch->offset = 0;

#if SOUND_LOW_PASS
    ch->alpha256 = 256u * 201u * sample_freq /
                   (201u * sample_freq + 64u * (unsigned)PICO_SOUND_SAMPLE_FREQ);
    ch->lp_last_sample = ch->decompressed[0];
#endif
    return true;
}

// ── Sound module callbacks ─────────────────────────────────────────────

static void GetSfxLumpName(const sfxinfo_t *sfx, char *buf, size_t buf_len) {
    if (sfx->link != NULL) sfx = sfx->link;
    if (use_sfx_prefix)
        M_snprintf(buf, buf_len, "ds%s", DEH_String(sfx->name));
    else
        M_StringCopy(buf, DEH_String(sfx->name), buf_len);
}

static void I_Pico_PrecacheSounds(should_be_const sfxinfo_t *sounds, int num_sounds) {}

static int I_Pico_GetSfxLumpNum(should_be_const sfxinfo_t *sfx) {
    char namebuf[9];
    GetSfxLumpName(sfx, namebuf, sizeof(namebuf));
    return W_GetNumForName(namebuf);
}

static void I_Pico_UpdateSoundParams(int handle, int vol, int sep) {
    int left, right;
    if (!sound_initialized || handle < 0 || handle >= NUM_SOUND_CHANNELS) return;
    left = ((254 - sep) * vol) / 127;
    right = ((sep) * vol) / 127;
    if (left < 0) left = 0; else if (left > 255) left = 255;
    if (right < 0) right = 0; else if (right > 255) right = 255;
    channels[handle].left = left;
    channels[handle].right = right;
}

static int I_Pico_StartSound(should_be_const sfxinfo_t *sfxinfo, int channel,
                             int vol, int sep, int pitch) {
    if (!check_and_init_channel(channel)) return -1;
    stop_channel(channel);
    channel_t *ch = &channels[channel];
    if (!init_channel_for_sfx(ch, sfxinfo, pitch)) {
        assert(!is_channel_playing(channel));
    }
    I_Pico_UpdateSoundParams(channel, vol, sep);
    return channel;
}

static void I_Pico_StopSound(int channel) {
    if (check_and_init_channel(channel)) {
        stop_channel(channel);
    }
}

static boolean I_Pico_SoundIsPlaying(int channel) {
    if (!check_and_init_channel(channel)) return false;
    return is_channel_playing(channel);
}

// ── Test Tone Generator ──────────────────────────────────────────────
// When AUDIO_TEST_TONES=1, replaces mixer output with a sequence of
// known test signals.  Full chain exercised: ring buf → SPI → P4 AA →
// resampler → ×8 gain → codec.  Record and analyze with
// tools/analyze_test_tones.py.
#if AUDIO_TEST_TONES

#define TT_RATE    44100   // Match PAB_SOURCE_FREQ — no P4 resampling
#define TT_2PI     6.28318530718f

// Int16 amplitudes for specific dBFS levels.
// P4 applies ×8 gain: clips when amp > 4096 (normalized > 0.125).
#define TT_AMP_M30  1036    // -30 dBFS → ×8 = 0.25 (clean)
#define TT_AMP_M24  2067    // -24 dBFS → ×8 = 0.50 (clean)
#define TT_AMP_M18  4125    // -18 dBFS → ×8 = 1.01 (edge clip)
#define TT_AMP_M12  8231    // -12 dBFS → ×8 = 2.01 (clips)

enum {
    TT_SYNC,            // 3 beeps at 2 kHz (alignment marker)
    TT_SILENCE,         // Noise floor measurement
    TT_1K_M30,          // 1 kHz -30 dBFS (clean, ×8 = 0.25)
    TT_1K_M24,          // 1 kHz -24 dBFS (clean, ×8 = 0.50)
    TT_1K_M18,          // 1 kHz -18 dBFS (edge of clipping)
    TT_1K_M12,          // 1 kHz -12 dBFS (clipped by ×8)
    TT_440_M24,         // 440 Hz -24 dBFS (tuning reference)
    TT_SWEEP,           // Log sweep 100 Hz → 20 kHz -24 dBFS
    TT_SQUARE_1K,       // 1 kHz square -24 dBFS (harmonics)
    TT_IMD,             // 19 kHz + 20 kHz -30 dBFS each (AA test)
    TT_IMPULSE,         // Impulse train 200 Hz -24 dBFS
    TT_DONE
};

static const char *tt_names[] = {
    "SYNC (3x beep 2kHz)",
    "SILENCE",
    "1kHz -30dBFS",
    "1kHz -24dBFS",
    "1kHz -18dBFS (clip edge)",
    "1kHz -12dBFS (clips)",
    "440Hz -24dBFS",
    "SWEEP 100Hz-20kHz -24dBFS",
    "SQUARE 1kHz -24dBFS",
    "IMD 19k+20k -30dBFS",
    "IMPULSE 200Hz -24dBFS",
    "DONE (silence)"
};

static int tt_current = -1;
static uint32_t tt_pos = 0;
static uint32_t tt_total_samples = 0;
static float tt_phase1 = 0.0f;
static float tt_phase2 = 0.0f;
static float tt_sweep_freq = 100.0f;
static float tt_sweep_step = 1.0f;

static uint32_t tt_duration(int phase) {
    switch (phase) {
        case TT_SYNC:   return TT_RATE * 1;        // 1 s
        case TT_SWEEP:  return TT_RATE * 8;        // 8 s
        default:        return TT_RATE * 4;        // 4 s
    }
}

static void tt_advance_phase(void) {
    tt_current++;
    tt_pos = 0;
    tt_phase1 = 0.0f;
    tt_phase2 = 0.0f;
    if (tt_current == TT_SWEEP) {
        tt_sweep_freq = 100.0f;
        tt_sweep_step = powf(20000.0f / 100.0f, 1.0f / (8.0f * TT_RATE));
    }
    if (tt_current <= TT_DONE) {
        printf("[TT] Phase %d: %s (at sample %lu)\n",
               tt_current, tt_names[tt_current],
               (unsigned long)tt_total_samples);
    }
}

static void tt_fill_buffer(int16_t *out, uint32_t count) {
    if (tt_current < 0) {
        tt_current = 0;
        tt_pos = 0;
        tt_total_samples = 0;
        tt_phase1 = 0.0f;
        tt_phase2 = 0.0f;
        tt_sweep_freq = 100.0f;
        printf("[TT] ═══ Audio Test Tone Sequence ═══\n");
        printf("[TT] Rate: %d Hz, mono (L=R), ~45s total\n", TT_RATE);
        printf("[TT] Phase 0: %s (at sample 0)\n", tt_names[0]);
    }

    for (uint32_t i = 0; i < count; i++) {
        int16_t val = 0;

        switch (tt_current) {
        case TT_SYNC: {
            // 3 beeps: 100ms on, 100ms off, at 2 kHz, amplitude 2048
            uint32_t seg = tt_pos / (TT_RATE / 10);
            if (seg < 6 && (seg & 1) == 0) {
                val = (int16_t)(2048.0f * sinf(tt_phase1 * TT_2PI));
                tt_phase1 += 2000.0f / TT_RATE;
                if (tt_phase1 >= 1.0f) tt_phase1 -= 1.0f;
            }
            break;
        }
        case TT_SILENCE:
            break;
        case TT_1K_M30:
            val = (int16_t)((float)TT_AMP_M30 * sinf(tt_phase1 * TT_2PI));
            tt_phase1 += 1000.0f / TT_RATE;
            if (tt_phase1 >= 1.0f) tt_phase1 -= 1.0f;
            break;
        case TT_1K_M24:
            val = (int16_t)((float)TT_AMP_M24 * sinf(tt_phase1 * TT_2PI));
            tt_phase1 += 1000.0f / TT_RATE;
            if (tt_phase1 >= 1.0f) tt_phase1 -= 1.0f;
            break;
        case TT_1K_M18:
            val = (int16_t)((float)TT_AMP_M18 * sinf(tt_phase1 * TT_2PI));
            tt_phase1 += 1000.0f / TT_RATE;
            if (tt_phase1 >= 1.0f) tt_phase1 -= 1.0f;
            break;
        case TT_1K_M12:
            val = (int16_t)((float)TT_AMP_M12 * sinf(tt_phase1 * TT_2PI));
            tt_phase1 += 1000.0f / TT_RATE;
            if (tt_phase1 >= 1.0f) tt_phase1 -= 1.0f;
            break;
        case TT_440_M24:
            val = (int16_t)((float)TT_AMP_M24 * sinf(tt_phase1 * TT_2PI));
            tt_phase1 += 440.0f / TT_RATE;
            if (tt_phase1 >= 1.0f) tt_phase1 -= 1.0f;
            break;
        case TT_SWEEP:
            val = (int16_t)((float)TT_AMP_M24 * sinf(tt_phase1 * TT_2PI));
            tt_phase1 += tt_sweep_freq / TT_RATE;
            if (tt_phase1 >= 1.0f) tt_phase1 -= 1.0f;
            tt_sweep_freq *= tt_sweep_step;
            break;
        case TT_SQUARE_1K:
            val = (tt_phase1 < 0.5f) ? TT_AMP_M24 : -TT_AMP_M24;
            tt_phase1 += 1000.0f / TT_RATE;
            if (tt_phase1 >= 1.0f) tt_phase1 -= 1.0f;
            break;
        case TT_IMD: {
            float s1 = (float)TT_AMP_M30 * sinf(tt_phase1 * TT_2PI);
            float s2 = (float)TT_AMP_M30 * sinf(tt_phase2 * TT_2PI);
            val = (int16_t)(s1 + s2);
            tt_phase1 += 19000.0f / TT_RATE;
            tt_phase2 += 20000.0f / TT_RATE;
            if (tt_phase1 >= 1.0f) tt_phase1 -= 1.0f;
            if (tt_phase2 >= 1.0f) tt_phase2 -= 1.0f;
            break;
        }
        case TT_IMPULSE: {
            uint32_t period = TT_RATE / 200;  // ~249 samples
            if ((tt_pos % period) == 0) val = TT_AMP_M24;
            break;
        }
        default:
            break;
        }

        out[i * 2] = val;       // L
        out[i * 2 + 1] = val;   // R (mono test)
        tt_pos++;
        tt_total_samples++;

        if (tt_current < TT_DONE && tt_pos >= tt_duration(tt_current)) {
            tt_advance_phase();
        }
    }
}

#endif // AUDIO_TEST_TONES

// Atomic flag to prevent Core 0 and Core 1 from calling the mixer
// simultaneously.  Core 1 calls SafeUpdateSound() from pd_render.cpp
// while waiting for semaphores (MULTICORE_RENDERING=1).
// Uses __sync_lock_test_and_set (LDREX/STREX on Cortex-M33) which is
// atomic across both cores via the global exclusive monitor.
// Does NOT disable interrupts — the TIMER1 ISR must keep firing.
static volatile uint32_t mix_lock_flag = 0;

static void I_Pico_UpdateSound(void) {
    if (!sound_initialized) return;

    // Atomic try-lock: sets flag to 1 and returns old value.
    // If old value was 1 (other core holds it), skip this call.
    if (__sync_lock_test_and_set(&mix_lock_flag, 1)) {
        return;
    }

    // Fill as many ring-buffer chunks as possible per call.
    // Called from BOTH cores (Core 1 via SafeUpdateSound during render).
    audio_buffer_t *buffer;
    while ((buffer = pab_take_buffer()) != NULL) {
#if AUDIO_TEST_TONES
        tt_fill_buffer((int16_t *)buffer->buffer->bytes, buffer->max_sample_count);
        buffer->sample_count = buffer->max_sample_count;
#else
        if (music_generator) {
            music_generator(buffer);
        } else {
            memset(buffer->buffer->bytes, 0, buffer->buffer->size);
        }

        // Music is already ×8 boosted (<<= 3 in opl_pico.c), matching
        // the original rp2040-doom.  SFX are mixed directly into the
        // same int16 buffer — exactly as the original does.  This
        // preserves the correct music/SFX volume ratio.
        for (int ch = 0; ch < NUM_SOUND_CHANNELS; ch++) {
            if (!is_channel_playing(ch)) continue;
            channel_t *channel = &channels[ch];
            assert(channel->decompressed_size);
            int voll = channel->left / 2;
            int volr = channel->right / 2;
            uint32_t offset_end = channel->decompressed_size * 65536;
            assert(channel->offset < offset_end);
            int16_t *samples = (int16_t *)buffer->buffer->bytes;
#if SOUND_LOW_PASS
            int alpha256 = channel->alpha256;
            int beta256 = 256 - alpha256;
            int sample = channel->lp_last_sample;
#endif
            for (uint32_t s = 0; s < buffer->max_sample_count; s++) {
#if !SOUND_LOW_PASS
                int sample = channel->decompressed[channel->offset >> 16];
#else
                sample = (beta256 * sample +
                          alpha256 * channel->decompressed[channel->offset >> 16]) / 256;
#endif
                *samples++ += sample * voll;
                *samples++ += sample * volr;
                channel->offset += channel->step;
                if (channel->offset >= offset_end) {
                    channel->offset -= offset_end;
                    decompress_buffer(channel);
                    offset_end = channel->decompressed_size * 65536;
                    if (channel->offset >= offset_end) {
                        stop_channel(ch);
                        break;
                    }
                }
            }
#if SOUND_LOW_PASS
            channel->lp_last_sample = sample;
#endif
        }

        buffer->sample_count = buffer->max_sample_count;

        // Fade handling
        if (fade_state == FS_SILENT) {
            memset(buffer->buffer->bytes, 0, buffer->buffer->size);
        } else if (fade_state != FS_NONE) {
            int16_t *samples = (int16_t *)buffer->buffer->bytes;
            int fade_step = fade_state == FS_FADE_IN ? FADE_STEP : -FADE_STEP;
            uint32_t i;
            for (i = 0; i < buffer->sample_count * 2 && fade_level; i += 2) {
                samples[i]     = (samples[i]     * (int)fade_level) >> 16;
                samples[i + 1] = (samples[i + 1] * (int)fade_level) >> 16;
                fade_level += fade_step;
            }
            if (!fade_level) {
                if (fade_state == FS_FADE_OUT) {
                    for (; i < buffer->sample_count * 2; i++)
                        samples[i] = 0;
                    fade_state = FS_SILENT;
                } else {
                    fade_state = FS_NONE;
                }
            }
        }
#endif // AUDIO_TEST_TONES

        pab_give_buffer(buffer);
    }

    __sync_lock_release(&mix_lock_flag);

    // Drain ring buffer → SPI (word-clock paced)
    // Only Core 0 should poll — Core 1 just fills the ring buffer.
    p4_spi_transport_poll();
}

static void I_Pico_ShutdownSound(void) {
    if (!sound_initialized) return;
    sound_initialized = false;
}

static boolean I_Pico_InitSound(boolean _use_sfx_prefix) {
    use_sfx_prefix = _use_sfx_prefix;
    pab_init();

    // Test tone: disabled. Enable with pab_set_test_tone(true) to debug.
    // pab_set_test_tone(true);

    // P4 control link is initialized in sd_wad_loader.c after SD card release

    printf("[DOOM] About to call p4_spi_transport_init()...\n");
    p4_spi_transport_init();
    printf("[DOOM] p4_spi_transport_init() returned OK\n");
    sound_initialized = true;
    printf("[DOOM] Sound initialized (SPI bridge, %d Hz)\n", PICO_SOUND_SAMPLE_FREQ);
    return true;
}

static snddevice_t sound_pico_devices[] = { SNDDEVICE_SB };

sound_module_t sound_pico_module = {
    sound_pico_devices,
    arrlen(sound_pico_devices),
    I_Pico_InitSound,
    I_Pico_ShutdownSound,
    I_Pico_GetSfxLumpNum,
    I_Pico_UpdateSound,
    I_Pico_UpdateSoundParams,
    I_Pico_StartSound,
    I_Pico_StopSound,
    I_Pico_SoundIsPlaying,
    I_Pico_PrecacheSounds,
};

bool I_PicoSoundIsInitialized(void) {
    return sound_initialized;
}

void I_PicoSoundSetMusicGenerator(void (*generator)(audio_buffer_t *buffer)) {
    music_generator = generator;
}

void I_PicoSoundFade(bool in) {
    fade_state = in ? FS_FADE_IN : FS_FADE_OUT;
    fade_level = in ? FADE_STEP : 0x10000 - FADE_STEP;
}

bool I_PicoSoundFading(void) {
    return fade_state == FS_FADE_IN || fade_state == FS_FADE_OUT;
}

// ── Flash stub (RP2350 has different flash HW, saves disabled anyway) ──

void picoflash_sector_program(uint32_t flash_offs, const uint8_t *data) {
    (void)flash_offs;
    (void)data;
}
