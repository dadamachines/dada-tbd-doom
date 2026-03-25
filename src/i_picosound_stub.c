// TBD-16 Doom — Sound stubs (audio deferred to P4 co-processor)
// Replaces i_picosound.c which requires pico-extras pico_audio_i2s
#include "config.h"
#include "doomtype.h"
#include "i_sound.h"
#include "picoflash.h"

typedef struct audio_buffer audio_buffer_t;

// --- Sound stubs ---

static boolean stub_init(boolean _use_sfx_prefix) { return false; }
static void stub_shutdown(void) {}
static int stub_getsfxlumpnum(should_be_const sfxinfo_t *sfx) { return 0; }
static void stub_update(void) {}
static void stub_updateparams(int handle, int vol, int sep) {}
static int stub_startsound(should_be_const sfxinfo_t *sfxinfo, int channel, int vol, int sep, int pitch) { return channel; }
static void stub_stopsound(int channel) {}
static boolean stub_isplaying(int channel) { return false; }
static void stub_precache(should_be_const sfxinfo_t *sounds, int num_sounds) {}

static snddevice_t sound_stub_devices[] = { SNDDEVICE_SB };

sound_module_t sound_pico_module = {
    sound_stub_devices,
    sizeof(sound_stub_devices) / sizeof(sound_stub_devices[0]),
    stub_init,
    stub_shutdown,
    stub_getsfxlumpnum,
    stub_update,
    stub_updateparams,
    stub_startsound,
    stub_stopsound,
    stub_isplaying,
    stub_precache,
};

bool I_PicoSoundIsInitialized(void) { return false; }
void I_PicoSoundSetMusicGenerator(void (*generator)(audio_buffer_t *buffer)) {}
void I_PicoSoundFade(bool in) {}
bool I_PicoSoundFading(void) { return false; }

// --- Flash stub (RP2350 has different flash HW, saves disabled anyway) ---

void picoflash_sector_program(uint32_t flash_offs, const uint8_t *data) {
    (void)flash_offs;
    (void)data;
}
