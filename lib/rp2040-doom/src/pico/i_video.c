//
// Copyright(C) 1993-1996 Id Software, Inc.
// Copyright(C) 2005-2014 Simon Howard
// Copyright(C) 2021-2022 Graham Sanderson
//
// This program is free software; you can redistribute it and/or
// modify it under the terms of the GNU General Public License
// as published by the Free Software Foundation; either version 2
// of the License, or (at your option) any later version.
//
// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU General Public License for more details.
//
// DESCRIPTION:
//	DOOM graphics stuff for Pico.
//

#if PICODOOM_RENDER_NEWHOPE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <limits.h>
#include <doom/r_data.h>
#include "doom/f_wipe.h"
#include "pico.h"

#include "config.h"
#include "d_loop.h"
#include "deh_str.h"
#include "doomtype.h"
#include "i_input.h"
#include "i_joystick.h"
#include "i_system.h"
#include "i_timer.h"
#include "i_video.h"
#include "m_argv.h"
#include "m_config.h"
#include "m_misc.h"
#include "tables.h"
#include "v_diskicon.h"
#include "v_video.h"
#include "w_wad.h"
#include "z_zone.h"

#if PICO_ON_DEVICE
#ifdef J_COLOUR_MOD
#define J_OLED_COLOUR
#else
#define J_OLED_MONO
#endif
#endif

#include "pico/multicore.h"
#include "pico/sync.h"
#include "pico/time.h"
#include "hardware/gpio.h"
#include "picodoom.h"
#if !JTBD16
#include "video_doom.pio.h"
#endif
#include "image_decoder.h"
#if PICO_ON_DEVICE
#include "hardware/dma.h"
#include "hardware/structs/xip_ctrl.h"
#include "hardware/spi.h"
#include "pio_spi_oled.h"
#else
#include "SDL_image.h"
#include "SDL_mutex.h"
#include "pico/scanvideo.h"
#include "pico/scanvideo/composable_scanline.h"
#endif

int debugline = 39;

#if defined J_OLED_MONO
#if JTBD16
#define DISPLAYWIDTH 128
#define DISPLAYHEIGHT 64
#else
#define DISPLAYWIDTH 72
#define DISPLAYHEIGHT 40
#endif
#elif defined J_OLED_COLOUR
#define DISPLAYWIDTH 96
#define DISPLAYHEIGHT 54
#else

#if FSAA
#define DISPLAYWIDTH (SCREENWIDTH>>FSAA)
#define DISPLAYHEIGHT (SCREENHEIGHT>>FSAA)
#else
#define DISPLAYWIDTH SCREENWIDTH
#define DISPLAYHEIGHT SCREENHEIGHT
#endif

#endif

#define YELLOW_SUBMARINE 0
#define SUPPORT_TEXT 1
#if SUPPORT_TEXT
typedef struct __packed {
    const char * const name;
    const uint8_t * const data;
    const uint8_t w;
    const uint8_t h;
} txt_font_t;
#define TXT_SCREEN_W 80
#include "fonts/normal.h"

#endif

// todo temproarly turned this off because it causes a seeming bug in scanvideo (perhaps only with the new callback stuff) where the last repeated scanline of a pixel line is freed while shown
//  note it may just be that this happens anyway, but usually we are writing slower than the beam?
#define USE_INTERP PICO_ON_DEVICE
#if USE_INTERP
#include "hardware/interp.h"
#endif


CU_REGISTER_DEBUG_PINS(scanline_copy)
//CU_SELECT_DEBUG_PINS(scanline_copy)

static const patch_t *stbar;

volatile uint8_t interp_in_use;


// display has been set up?

static boolean initialized = false;

boolean screenvisible = true;

//int vga_porch_flash = false;

//static int startup_delay = 1000;

// The screen buffer; this is modified to draw things to the screen
//pixel_t *I_VideoBuffer = NULL;
// Gamma correction level to use

boolean screensaver_mode = false;

isb_int8_t usegamma = 0;

// Joystick/gamepad hysteresis
unsigned int joywait = 0;

pixel_t *I_VideoBuffer; // todo can't have this

uint8_t __aligned(4) frame_buffer[2][SCREENWIDTH * SCREENHEIGHT];
#ifdef J_OLED_COLOUR
static uint16_t display_palette[256];
#else
static uint8_t display_palette[256];
#endif
static uint8_t __scratch_x("shared_pal") shared_pal[NUM_SHARED_PALETTES][16];
static int8_t next_pal=-1;

semaphore_t vsync;

uint8_t *text_screen_data;
static uint32_t *text_scanline_buffer_start;
static uint8_t *text_screen_cpy;
static uint8_t *text_font_cpy;


#if USE_INTERP
static interp_hw_save_t interp0_save, interp1_save;
static boolean interp_updated;
static boolean need_save;

static inline void interp_save_static(interp_hw_t *interp, interp_hw_save_t *saver) {
    saver->accum[0] = interp->accum[0];
    saver->accum[1] = interp->accum[1];
    saver->base[0] = interp->base[0];
    saver->base[1] = interp->base[1];
    saver->base[2] = interp->base[2];
    saver->ctrl[0] = interp->ctrl[0];
    saver->ctrl[1] = interp->ctrl[1];
}

static inline void interp_restore_static(interp_hw_t *interp, interp_hw_save_t *saver) {
    interp->accum[0] = saver->accum[0];
    interp->accum[1] = saver->accum[1];
    interp->base[0] = saver->base[0];
    interp->base[1] = saver->base[1];
    interp->base[2] = saver->base[2];
    interp->ctrl[0] = saver->ctrl[0];
    interp->ctrl[1] = saver->ctrl[1];
}
#endif

void I_ShutdownGraphics(void)
{
}

//
// I_StartFrame
//
void I_StartFrame (void)
{
    // er?
}

//
// Set the window title
//

void I_SetWindowTitle(const char *title)
{
//    window_title = title;
}

//
// I_SetPalette
//
void I_SetPaletteNum(int doompalette)
{
    next_pal = doompalette;
}


uint8_t display_frame_index;
uint8_t display_overlay_index;
uint8_t display_video_type;


uint8_t *wipe_yoffsets; // position of start of y in each column
int16_t *wipe_yoffsets_raw;
uint32_t *wipe_linelookup; // offset of each line from start of screenbuffer (can be negative for FB 1 to FB 0)
uint8_t next_video_type;
uint8_t next_frame_index; // todo combine with video type?
uint8_t next_overlay_index;
#if !DEMO1_ONLY
uint8_t *next_video_scroll;
uint8_t *video_scroll;
#endif
volatile uint8_t wipe_min;

#pragma GCC push_options
#if PICO_ON_DEVICE
#pragma GCC optimize("O3")
#endif

#ifdef J_OLED_COLOUR

static inline uint16_t display_rgb(uint8_t r, uint8_t g, uint8_t b) {
    return (r & 0b11111000) | (g >> 5) | ((g & 0b00011100) << 11) | ((b & 0b11111000) << 5);
}

#else

static inline uint8_t display_rgb(uint8_t r, uint8_t g, uint8_t b) {
    // ITU-R BT.601 perceptual luminance (0.299R + 0.587G + 0.114B)
    // approximated as (77R + 150G + 29B) >> 8
    uint lum = (r * 77 + g * 150 + b * 29) >> 8;
    if (lum > 255) lum = 255;
    return lum;
}

#endif

// this is not in flash as quite large and only once per frame
void __noinline new_frame_init_overlays_palette_and_wipe() {
    // re-initialize our overlay drawing
    if (display_video_type >= FIRST_VIDEO_TYPE_WITH_OVERLAYS) {
        memset(vpatchlists->vpatch_next, 0, sizeof(vpatchlists->vpatch_next));
        memset(vpatchlists->vpatch_starters, 0, sizeof(vpatchlists->vpatch_starters));
        memset(vpatchlists->vpatch_doff, 0, sizeof(vpatchlists->vpatch_doff));
        vpatchlist_t *overlays = vpatchlists->overlays[display_overlay_index];
        // do it in reverse so our linked lists are in ascending order
        for (int i = overlays->header.size - 1; i > 0; i--) {
            assert(overlays[i].entry.y < count_of(vpatchlists->vpatch_starters));
            vpatchlists->vpatch_next[i] = vpatchlists->vpatch_starters[overlays[i].entry.y];
            vpatchlists->vpatch_starters[overlays[i].entry.y] = i;
        }
        if (next_pal != -1) {
            static const uint8_t *playpal;
            static bool calculate_palettes;
            if (!playpal) {
                lumpindex_t l = W_GetNumForName("PLAYPAL");
                playpal = W_CacheLumpNum(l, PU_STATIC);
                calculate_palettes = W_LumpLength(l) == 768;
            }
            if (!calculate_palettes || !next_pal) {
                const uint8_t *doompalette = playpal + next_pal * 768;
                for (int i = 0; i < 256; i++) {
                    int r = *doompalette++;
                    int g = *doompalette++;
                    int b = *doompalette++;

                    if (usegamma) {
                        r = gammatable[usegamma-1][r];
                        g = gammatable[usegamma-1][g];
                        b = gammatable[usegamma-1][b];
                    }

                    display_palette[i] = display_rgb(r, g, b);
                }
            } else {
                int mul, r0, g0, b0;
                if (next_pal < 9) {
                    mul = next_pal * 65536 / 9;
                    r0 = 255; g0 = b0 = 0;
                } else if (next_pal < 13) {
                    mul = (next_pal - 8) * 65536 / 8;
                    r0 = 215; g0 = 186; b0 = 69;
                } else {
                    mul = 65536 / 8;
                    r0 = b0 = 0; g0 = 256;
                }
                const uint8_t *doompalette = playpal;
                for (int i = 0; i < 256; i++) {
                    int r = *doompalette++;
                    int g = *doompalette++;
                    int b = *doompalette++;

                    r += ((r0 - r) * mul) >> 16;
                    g += ((g0 - g) * mul) >> 16;
                    b += ((b0 - b) * mul) >> 16;

                    display_palette[i] = display_rgb(r, g, b);
                }
            }
            next_pal = -1;
            assert(vpatch_type(stbar) == vp4_solid); // no transparent, no runs, 4 bpp
            for (int i = 0; i < NUM_SHARED_PALETTES; i++) {
                patch_t *patch = resolve_vpatch_handle(vpatch_for_shared_palette[i]);
                assert(vpatch_colorcount(patch) <= 16);
                assert(vpatch_has_shared_palette(patch));
                for (int j = 0; j < 16; j++) {
                    shared_pal[i][j] = display_palette[vpatch_palette(patch)[j]];
                }
            }
        }
        if (display_video_type == VIDEO_TYPE_WIPE) {
            printf("WIPEMIN %d\n", wipe_min);
            if (wipe_min <= 200) {
                bool regular = display_overlay_index; // just happens to toggle every frame
                int new_wipe_min = 200;
                for (int i = 0; i < SCREENWIDTH; i++) {
                    int v;
                    if (wipe_yoffsets_raw[i] < 0) {
                        if (regular) {
                            wipe_yoffsets_raw[i]++;
                        }
                        v = 0;
                    } else {
                        int dy = (wipe_yoffsets_raw[i] < 16) ? (1 + wipe_yoffsets_raw[i] + regular) / 2 : 4;
                        if (wipe_yoffsets_raw[i] + dy > 200) {
                            v = 200;
                        } else {
                            wipe_yoffsets_raw[i] += dy;
                            v = wipe_yoffsets_raw[i];
                        }
                    }
                    wipe_yoffsets[i] = v;
                    if (v < new_wipe_min) new_wipe_min = v;
                }
                assert(new_wipe_min >= wipe_min);
                wipe_min = new_wipe_min;
            }
        }
    }
}

static void display_update(const uint8_t* const frame);

//
// I_FinishUpdate
//
void I_FinishUpdate (void)
{
#ifndef J_OLED_COLOUR
    sem_acquire_blocking(&vsync);
#endif
    display_video_type = next_video_type;
    display_frame_index = next_frame_index;
    display_overlay_index = next_overlay_index;

    if (display_video_type != VIDEO_TYPE_SAVING) {
        // this stuff is large (so in flash) and not needed in save move
        new_frame_init_overlays_palette_and_wipe();
    }

#ifdef J_OLED_COLOUR
    display_update(frame_buffer[display_frame_index]);
#else
    sem_release(&vsync);
#endif
}

#pragma GCC pop_options

#if PICO_ON_DEVICE
#define LOW_PRIO_IRQ 31
#include "hardware/irq.h"

static void __not_in_flash_func(free_buffer_callback)() {
//    irq_set_pending(LOW_PRIO_IRQ);
    // ^ is in flash by default
    // Use irq_set_pending for RP2040/RP2350 portability
    irq_set_pending(LOW_PRIO_IRQ);
}

#ifdef J_OLED_MONO

#ifdef J_OLED_FRAME_PERIOD
#define FRAME_PERIOD J_OLED_FRAME_PERIOD
#else
#define FRAME_PERIOD 6600
#endif

// some oleds need 2 park lines, but that's not as robust44
#define PARK_LINES 2

#if JTBD16
// SSD1309 128x64 init sequence (TBD-16 optimized for PIO SPI + datasheet UG-2864AxxPG01)
static const uint8_t command_initialise[] = {
    0xFD, 0x12,     // command lock (unlock)
    0xAE,           // display off
    0xD5, 0xF0,     // set display clock divide — max oscillator for fastest internal refresh (~118 Hz)
    0xA8, DISPLAYHEIGHT-1, // set multiplex ratio (63)
    0xD3, 0x00,     // set display offset
    0x40,           // set display start line 0
    0x20, 0x00,     // set horizontal addressing mode
    0xA1,           // set segment remap (mirrored — compensates for 180° HW mounting)
    0xC8,           // set COM scan direction (remapped — hardware Y-flip, eliminates per-pixel software flip)
    0xDA, 0x12,     // set COM pins config (alternate)
    0x81, 0xDF,     // set contrast (datasheet recommended max for UG-2864AxxPG01)
    0xD9, 0x82,     // set pre-charge period (TBD-16 tuned)
    0xDB, 0x34,     // set VCOMH deselect (TBD-16 tuned)
    0xA4,           // display follows RAM
    0xA6,           // normal display
    0x21, 0x00, 0x7F, // set column address 0-127
    0x22, 0x00, 0x07, // set page address 0-7
    0xAF            // display on
};

static const uint8_t command_park[] = {
    // Reset GRAM write pointer to page 0, col 0.
    // No MUX parking possible: all 64 COM lines are visible on SSD1309 128x64.
    0x21, 0x00, 0x7F,     // column address 0-127
    0x22, 0x00, 0x07,     // page address 0-7
};

static uint8_t command_run[] = {
    0x81, 1,              // set contrast level (updated per sub-frame)
};

static const uint8_t contrast[3] = {
    0xDF, 0x5F, 0x17  // SSD1309 greyscale levels (tuned for TBD-16)
};

// SPI for OLED now uses PIO — see pio_spi_oled.c

#else  // !JTBD16 — original SSD1306 72×40
    0xAE,           //display off
    0xD5, 0xF0,     //set display clock divide
    0xA8, DISPLAYHEIGHT-1, //set multiplex ratio 39
    0xD3, 0x00,     //set display offset
    0x40,           //set display start line 0
    0x8D, 0x14,     //set charge pump enabled (0x14:7.5v 0x15:6.0v 0x94:8.5v 0x95:9.0v)
    0x20, 0x00,     //set addressing mode horizontal
    0xA1,           //set segment remap (0=seg0)
    0xC0,           //set com scan direction
    0xDA, 0x12,     //set alternate com pin configuration
    0xAD, 0x30,     //internal iref enabled (0x30:240uA 0x10:150uA)
    0x81, 0x01,     //set contrast
    0xD9, 0x11,     //set pre-charge period
    0xDB, 0x20,     //set vcomh deselect
    0xA4,           //unset entire display on
    0xA6,           //unset inverse display
    0x21, 28, 99,   //set column address / start 28 / end 99
    0x22, 0, 4,     //set page address / start 0 / end 4
    0xAF            // set display on
};

static const uint8_t command_park[] = {
    0xA8, PARK_LINES - 1,        //set 2-line multiplex
    0xD3, 4         //set display offset off the... bottom?
};

static uint8_t command_run[] = {
    0x81, 1,        //set level
    0xD3, 0,        //reset display offset
    0xA8, DISPLAYHEIGHT + 16 - 1,       //multiplex + overscan
};

static const uint8_t contrast[3] = {
    0x7f, 0x1f, 0x07
};

#define TBD_SPI spi0

#endif // JTBD16

uint8_t field_buffer[DISPLAYWIDTH*(DISPLAYHEIGHT/8)] = {};

static volatile bool core1_active = false;

// Simple 3x5 digit font for debug display (digits 0-9)
static const uint8_t debug_digits[10][5] = {
    {0x07,0x05,0x05,0x05,0x07}, // 0
    {0x02,0x06,0x02,0x02,0x07}, // 1
    {0x07,0x01,0x07,0x04,0x07}, // 2
    {0x07,0x01,0x07,0x01,0x07}, // 3
    {0x05,0x05,0x07,0x01,0x01}, // 4
    {0x07,0x04,0x07,0x01,0x07}, // 5
    {0x07,0x04,0x07,0x05,0x07}, // 6
    {0x07,0x01,0x02,0x04,0x04}, // 7
    {0x07,0x05,0x07,0x05,0x07}, // 8
    {0x07,0x05,0x07,0x01,0x07}, // 9
};

// Show a single digit (0-9) on the OLED at the current position
// Call after power_on_logo() has initialized SPI
void debug_show_stage(int stage) {
#if JTBD16
    if (!JTBD16_BOOT_DEBUG) return;
    if (core1_active) return;
    // Set page 0, column 0 (command mode)
    gpio_put(J_OLED_CS, 1);
    gpio_put(J_OLED_DC, 0);
    gpio_put(J_OLED_CS, 0);
    uint8_t pos_cmd[] = {0x21, 0x00, 0x7F, 0x22, 0x00, 0x00}; // col 0-127, page 0 only
    oled_spi_write_blocking(pos_cmd, sizeof(pos_cmd));

    // Write digit bitmap (data mode)
    gpio_put(J_OLED_CS, 1);
    gpio_put(J_OLED_DC, 1);
    gpio_put(J_OLED_CS, 0);

    // Each column in the digit is a vertical 5-pixel strip packed into one byte
    uint8_t col_data[128] = {0};
    int d = stage % 10;
    for (int col = 0; col < 3; col++) {
        uint8_t byte = 0;
        for (int row = 0; row < 5; row++) {
            if (debug_digits[d][row] & (0x04 >> col))
                byte |= (1 << row);
        }
        // Scale up 4x for visibility
        uint8_t scaled = 0;
        for (int b = 0; b < 5; b++) {
            if (byte & (1 << b)) {
                scaled |= (1 << b);
            }
        }
        col_data[col*4]   = scaled;
        col_data[col*4+1] = scaled;
        col_data[col*4+2] = scaled;
        col_data[col*4+3] = scaled;
    }
    oled_spi_write_blocking(col_data, 128);
#endif
}

// Show a hex value on OLED page 1 (8 hex digits)
void debug_show_hex(uint32_t val) {
#if JTBD16
    if (!JTBD16_BOOT_DEBUG) return;
    if (core1_active) return;
    static const uint8_t hex_font[16][3] = {
        {0x1F,0x11,0x1F},{0x00,0x1F,0x00},{0x1D,0x15,0x17},{0x15,0x15,0x1F},
        {0x07,0x04,0x1F},{0x17,0x15,0x1D},{0x1F,0x15,0x1D},{0x01,0x01,0x1F},
        {0x1F,0x15,0x1F},{0x17,0x15,0x1F},{0x1F,0x05,0x1F},{0x1F,0x14,0x1F},
        {0x1F,0x11,0x11},{0x1F,0x10,0x1F},{0x1F,0x15,0x11},{0x1F,0x05,0x01},
    };
    gpio_put(J_OLED_CS, 1);
    gpio_put(J_OLED_DC, 0);
    gpio_put(J_OLED_CS, 0);
    uint8_t pos_cmd[] = {0x21, 0x00, 0x7F, 0x22, 0x01, 0x01}; // page 1
    oled_spi_write_blocking(pos_cmd, sizeof(pos_cmd));
    gpio_put(J_OLED_CS, 1);
    gpio_put(J_OLED_DC, 1);
    gpio_put(J_OLED_CS, 0);
    uint8_t col_data[128] = {0};
    for (int i = 0; i < 8; i++) {
        int nibble = (val >> (28 - i*4)) & 0xF;
        int x = i * 16;
        for (int col = 0; col < 3; col++) {
            col_data[x + col*4]     = hex_font[nibble][col];
            col_data[x + col*4 + 1] = hex_font[nibble][col];
            col_data[x + col*4 + 2] = hex_font[nibble][col];
            col_data[x + col*4 + 3] = hex_font[nibble][col];
        }
    }
    oled_spi_write_blocking(col_data, 128);
#endif
}

// Show hex on page 2
void debug_show_hex2(uint32_t val) {
#if JTBD16
    if (!JTBD16_BOOT_DEBUG) return;
    if (core1_active) return;
    static const uint8_t hex_font[16][3] = {
        {0x1F,0x11,0x1F},{0x00,0x1F,0x00},{0x1D,0x15,0x17},{0x15,0x15,0x1F},
        {0x07,0x04,0x1F},{0x17,0x15,0x1D},{0x1F,0x15,0x1D},{0x01,0x01,0x1F},
        {0x1F,0x15,0x1F},{0x17,0x15,0x1F},{0x1F,0x05,0x1F},{0x1F,0x14,0x1F},
        {0x1F,0x11,0x11},{0x1F,0x10,0x1F},{0x1F,0x15,0x11},{0x1F,0x05,0x01},
    };
    gpio_put(J_OLED_CS, 1);
    gpio_put(J_OLED_DC, 0);
    gpio_put(J_OLED_CS, 0);
    uint8_t pos_cmd[] = {0x21, 0x00, 0x7F, 0x22, 0x02, 0x02}; // page 2
    oled_spi_write_blocking(pos_cmd, sizeof(pos_cmd));
    gpio_put(J_OLED_CS, 1);
    gpio_put(J_OLED_DC, 1);
    gpio_put(J_OLED_CS, 0);
    uint8_t col_data[128] = {0};
    for (int i = 0; i < 8; i++) {
        int nibble = (val >> (28 - i*4)) & 0xF;
        int x = i * 16;
        for (int col = 0; col < 3; col++) {
            col_data[x + col*4]     = hex_font[nibble][col];
            col_data[x + col*4 + 1] = hex_font[nibble][col];
            col_data[x + col*4 + 2] = hex_font[nibble][col];
            col_data[x + col*4 + 3] = hex_font[nibble][col];
        }
    }
    oled_spi_write_blocking(col_data, 128);
#endif
}

uint8_t byte_reverse(uint8_t b) {
   b = (b & 0xF0) >> 4 | (b & 0x0F) << 4;
   b = (b & 0xCC) >> 2 | (b & 0x33) << 2;
   b = (b & 0xAA) >> 1 | (b & 0x55) << 1;
   return b;
}

void power_on_logo(void) {
    // get the logo up on screen using conservative settings and no greyscale trickery, as proof of life

    gpio_init(J_OLED_CS);
    gpio_set_dir(J_OLED_CS, GPIO_OUT);
    gpio_put(J_OLED_CS, 0);

    gpio_init(J_OLED_RESET);
    gpio_set_dir(J_OLED_RESET, GPIO_OUT);
    gpio_put(J_OLED_RESET, 0);

    gpio_init(J_OLED_DC);
    gpio_set_dir(J_OLED_DC, GPIO_OUT);
    gpio_put(J_OLED_DC, 0);

    gpio_put(J_OLED_RESET, 1);
    sleep_ms(1);
    gpio_put(J_OLED_RESET, 0);
    sleep_ms(10);
    gpio_put(J_OLED_RESET, 1);

    oled_spi_init();

    gpio_put(J_OLED_CS, 1);
    gpio_put(J_OLED_DC, 0);
    gpio_put(J_OLED_CS, 0);

#if JTBD16
    // SSD1309 128x64 init for power-on logo
    const uint8_t cmd_init[] = {
        0xFD, 0x12, 0xAE, 0xD5, 0xF0, 0xA8, 0x3F, 0xD3, 0x00, 0x40,
        0x20, 0x00, 0xA1, 0xC8, 0xDA, 0x12, 0x81, 0xDF, 0xD9, 0x82,
        0xDB, 0x34, 0xA4, 0xA6, 0x21, 0x00, 0x7F, 0x22, 0x00, 0x07, 0xAF
    };
    oled_spi_write_blocking(cmd_init, sizeof(cmd_init));

    // Clear screen (1024 bytes = 128x64/8)
    gpio_put(J_OLED_CS, 1);
    gpio_put(J_OLED_DC, 1);
    gpio_put(J_OLED_CS, 0);

    uint8_t zeros[128] = {0};
    for (int p = 0; p < 8; p++) {
        oled_spi_write_blocking(zeros, 128);
    }
#else
    const uint8_t cmd_init[] = {
        0xAE, 0x20, 0x00, 0x40, 0xA1, 0xA8, 0x27, 0xC8, 0xD3, 0x00, 0xDA, 0x12, 0xD5, 0xF0, 0xD9, 0x11, 0xDB, 0x20, 0x81, 0x7F,
        0xA4, 0xA6, 0x8D, 0x14, 0xAD, 0x30, 0x21, 0x1C, 0x63, 0x22, 0x00, 0x04, 0xAF
    };
    oled_spi_write_blocking(cmd_init, sizeof(cmd_init));

    gpio_put(J_OLED_CS, 1);
    gpio_put(J_OLED_DC, 1);
    gpio_put(J_OLED_CS, 0);

    const uint8_t dat_logo[] = {
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x02, 0xfe, 0x02, 0x02, 0x02, 0xe2, 0x22, 0x22, 0x22, 0xe2, 0x02, 0x02, 0x04, 0xfc,
        0x00, 0xfc, 0x02, 0x02, 0x02, 0xe2, 0x22, 0x22, 0x22, 0xe2, 0x02, 0x02, 0x02, 0xfc, 0x00, 0xfc, 0x02, 0x02, 0x02, 0xe2,
        0x22, 0x22, 0x22, 0xe2, 0x02, 0x02, 0x02, 0xfc, 0x00, 0x02, 0xfe, 0x02, 0x02, 0x02, 0x0c, 0x30, 0xc0, 0xc0, 0x30, 0x0c,
        0x02, 0x02, 0x02, 0x02, 0xfe, 0x02, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0xff,
        0x00, 0x00, 0x00, 0xff, 0x00, 0x00, 0x00, 0xff, 0x40, 0xa0, 0x00, 0xff, 0x00, 0xff, 0x00, 0x40, 0x80, 0xff, 0x00, 0x00,
        0x00, 0xff, 0x00, 0x00, 0x00, 0xff, 0x00, 0xff, 0x00, 0x00, 0x00, 0xff, 0x00, 0x00, 0x00, 0xff, 0x00, 0x00, 0x00, 0xff,
        0x00, 0x00, 0xff, 0xa0, 0x40, 0xf0, 0x80, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x80, 0x00, 0x00, 0xff, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0xff, 0x00, 0xa8, 0x54, 0xbf, 0x60, 0xb0, 0x58, 0xaf,
        0x55, 0x2a, 0x80, 0xff, 0x00, 0x7f, 0xc0, 0x95, 0x2a, 0x57, 0xa8, 0x50, 0xa0, 0x7f, 0x28, 0x90, 0xc0, 0x7f, 0x00, 0x7f,
        0xc0, 0x90, 0x28, 0x7f, 0xa0, 0x50, 0xa8, 0x57, 0x2a, 0x95, 0xc0, 0x7f, 0x00, 0x80, 0xff, 0x2a, 0x55, 0xff, 0x03, 0x3c,
        0xd5, 0x2a, 0xd4, 0x38, 0x07, 0xff, 0x50, 0xa0, 0xff, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0xff, 0x00, 0x2a, 0x95, 0xca, 0x65, 0x32, 0x19, 0x0c, 0x06, 0x03, 0x01, 0x00, 0x00, 0x00, 0x00, 0x01,
        0x03, 0x06, 0x0c, 0x19, 0x0c, 0x06, 0x03, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01, 0x03, 0x06, 0x0c, 0x19, 0x0c, 0x06,
        0x03, 0x01, 0x00, 0x00, 0x00, 0x00, 0x01, 0x03, 0x06, 0x0f, 0x00, 0x00, 0x03, 0x0c, 0x03, 0x00, 0x00, 0xff, 0x55, 0xaa,
        0xff, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x0f, 0x06, 0x03, 0x01, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01, 0x03, 0x06, 0x0f, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00
    };
        
    oled_spi_write_blocking(dat_logo, sizeof(dat_logo));
#endif // JTBD16

    gpio_put(J_OLED_CS, 1);

    //sleep_ms(2000);
}

static void display_driver_init() {

    gpio_init(J_OLED_CS);
    gpio_set_dir(J_OLED_CS, GPIO_OUT);
    gpio_put(J_OLED_CS, 0);

    gpio_init(J_OLED_RESET);
    gpio_set_dir(J_OLED_RESET, GPIO_OUT);
    gpio_put(J_OLED_RESET, 0);

    gpio_init(J_OLED_DC);
    gpio_set_dir(J_OLED_DC, GPIO_OUT);
    gpio_put(J_OLED_DC, 0);

    gpio_put(J_OLED_RESET, 0);
    sleep_ms(1);
    gpio_put(J_OLED_RESET, 1);
    sleep_ms(10);

    oled_spi_init();

    gpio_put(J_OLED_CS, 1);
    gpio_put(J_OLED_DC, 0);
    gpio_put(J_OLED_CS, 0);

    oled_spi_write_blocking(command_initialise, sizeof(command_initialise));

    gpio_put(J_OLED_CS, 1);
}

#elif defined J_OLED_COLOUR

#define FRAME_PERIOD 6600

static const uint8_t TFT_NOP = 0x00;
static const uint8_t TFT_SWRESET = 0x01;
static const uint8_t TFT_RDDID = 0x04;
static const uint8_t TFT_RDDST = 0x09;

static const uint8_t TFT_SLPIN  = 0x10;
static const uint8_t TFT_SLPOUT  = 0x11;
static const uint8_t TFT_PTLON  = 0x12;
static const uint8_t TFT_NORON  = 0x13;

static const uint8_t TFT_INVOFF = 0x20;
static const uint8_t TFT_INVON = 0x21;
static const uint8_t TFT_DISPOFF = 0x28;
static const uint8_t TFT_DISPON = 0x29;
static const uint8_t TFT_CASET = 0x2A;
static const uint8_t TFT_RASET = 0x2B;
static const uint8_t TFT_RAMWR = 0x2C;
static const uint8_t TFT_RAMRD = 0x2E;

static const uint8_t TFT_VSCRDEF = 0x33;
static const uint8_t TFT_VSCSAD = 0x37;

static const uint8_t TFT_COLMOD = 0x3A;
static const uint8_t TFT_MADCTL = 0x36;

static const uint8_t TFT_FRMCTR1 = 0xB1;
static const uint8_t TFT_FRMCTR2 = 0xB2;
static const uint8_t TFT_FRMCTR3 = 0xB3;
static const uint8_t TFT_INVCTR = 0xB4;
static const uint8_t TFT_DISSET5 = 0xB6;

static const uint8_t TFT_PWCTR1 = 0xC0;
static const uint8_t TFT_PWCTR2 = 0xC1;
static const uint8_t TFT_PWCTR3 = 0xC2;
static const uint8_t TFT_PWCTR4 = 0xC3;
static const uint8_t TFT_PWCTR5 = 0xC4;
static const uint8_t TFT_VMCTR1 = 0xC5;

static const uint8_t TFT_RDID1 = 0xDA;
static const uint8_t TFT_RDID2 = 0xDB;
static const uint8_t TFT_RDID3 = 0xDC;
static const uint8_t TFT_RDID4 = 0xDD;

static const uint8_t TFT_PWCTR6 = 0xFC;

static const uint8_t TFT_GMCTRP1 = 0xE0;
static const uint8_t TFT_GMCTRN1 = 0xE1;


static void write_cmd(uint8_t cmd) {
    gpio_put(J_OLED_DC, 0);
    gpio_put(J_OLED_CS, 0);
    spi_write_blocking(spi0, &cmd, 1);
    gpio_put(J_OLED_CS, 1);
}

static void write_data(const uint8_t* data, size_t len) {
    gpio_put(J_OLED_DC, 1);
    gpio_put(J_OLED_CS, 0);
    spi_write_blocking(spi0, data, len);
    gpio_put(J_OLED_CS, 1);
}


#define WRITE_DATA(...) {\
    const uint8_t databuf[] = { __VA_ARGS__ };\
    write_data(databuf, sizeof(databuf));\
}

static void display_reset(void) {
    gpio_put(J_OLED_DC, 0);
    gpio_put(J_OLED_RESET, 1);
    sleep_us(500);
    gpio_put(J_OLED_RESET, 0);
    sleep_us(500);
    gpio_put(J_OLED_RESET, 1);
    sleep_us(500);
}

static void display_driver_init(void) {
    gpio_init(J_OLED_CS);
    gpio_set_dir(J_OLED_CS, GPIO_OUT);
    gpio_put(J_OLED_CS, 0);

    gpio_init(J_OLED_RESET);
    gpio_set_dir(J_OLED_RESET, GPIO_OUT);
    gpio_put(J_OLED_RESET, 0);

    gpio_init(J_OLED_DC);
    gpio_set_dir(J_OLED_DC, GPIO_OUT);
    gpio_put(J_OLED_DC, 0);

    gpio_set_function(PICO_DEFAULT_SPI_SCK_PIN, GPIO_FUNC_SPI);
    gpio_set_function(PICO_DEFAULT_SPI_TX_PIN, GPIO_FUNC_SPI);
    spi_init(spi0, 60000000);
    spi_set_format(spi0, 8, SPI_CPOL_0, SPI_CPHA_0, SPI_MSB_FIRST);
    
    display_reset();

    write_cmd(TFT_SWRESET);
    sleep_us(150);
    write_cmd(TFT_SLPOUT);
    sleep_us(500);

    write_cmd(TFT_FRMCTR1);
    WRITE_DATA(0x01, 0x2C, 0x2D);

    write_cmd(TFT_FRMCTR2);
    WRITE_DATA(0x01, 0x2C, 0x2D);

    write_cmd(TFT_FRMCTR3);
    WRITE_DATA(0x01, 0x2c, 0x2d, 0x01, 0x2c, 0x2d);
    sleep_us(10);

    write_cmd(TFT_INVCTR);
    WRITE_DATA(0x07);

    write_cmd(TFT_PWCTR1);
    WRITE_DATA(0xA2, 0x02, 0x84);

    write_cmd(TFT_PWCTR2);
    WRITE_DATA(0xC5);

    write_cmd(TFT_PWCTR3);
    WRITE_DATA(0x0A, 0x00);

    write_cmd(TFT_PWCTR4);
    WRITE_DATA(0x8A, 0x2A);

    write_cmd(TFT_PWCTR5);
    WRITE_DATA(0x8A, 0xEE);

    write_cmd(TFT_VMCTR1);
    WRITE_DATA(0x0E);

    write_cmd(TFT_INVOFF);

    write_cmd(TFT_MADCTL);
    WRITE_DATA(0xC8);

    write_cmd(TFT_COLMOD);
    WRITE_DATA(0x05);

    write_cmd(TFT_CASET);
    WRITE_DATA(0x00, 16, 0x00, 16 + DISPLAYWIDTH - 1);

    write_cmd(TFT_RASET);
    WRITE_DATA(0x00, 106, 0x00, 106 + DISPLAYHEIGHT - 1);

    write_cmd(TFT_GMCTRP1);
    WRITE_DATA(0x0f, 0x1a, 0x0f, 0x18, 0x2f, 0x28, 0x20, 0x22, 0x1f, 0x1b, 0x23, 0x37, 0x00, 0x07, 0x02, 0x10);

    write_cmd(TFT_GMCTRN1);
    WRITE_DATA(0x0f, 0x1b, 0x0f, 0x17, 0x33, 0x2c, 0x29, 0x2e, 0x30, 0x30, 0x39, 0x3f, 0x00, 0x07, 0x03, 0x10);
    sleep_us(10);

    write_cmd(TFT_DISPON);
    sleep_us(100);

    write_cmd(TFT_NORON);
    sleep_us(10);

}

static uint16_t tft_buffer[DISPLAYWIDTH * DISPLAYHEIGHT];

void power_on_logo(void) {
    display_driver_init();
    
    
    write_cmd(TFT_RAMWR);
    write_data((const uint8_t*)tft_buffer, sizeof(tft_buffer));
}

static void display_update(const uint8_t* const frame) {

    for (int y = 0; y < DISPLAYHEIGHT; ++y) {
        for (int x = 0; x < DISPLAYWIDTH; ++x) {
            tft_buffer[y * DISPLAYWIDTH + x] = display_palette[frame[y*SCREENWIDTH + x]];
        }
    }

    write_cmd(TFT_RAMWR);
    write_data((const uint8_t*)tft_buffer, sizeof(tft_buffer));

}

#endif

#else

//this is pure laziness - using bits of pico-host-sdl's scanline simulation instead of setting up a clean sdl loop.
extern SDL_Surface *pico_access_surface;
extern SDL_Texture *texture_raw;
extern void send_update_screen();

const struct scanvideo_pio_program bogus_pio = {
    .id = "bogus"
};

static const scanvideo_timing_t bogus_timing = {
    .clock_freq = 1,
    .h_active = DISPLAYWIDTH,
    .v_active = DISPLAYHEIGHT,
    .h_total = 1,
    .v_total = 1,
};

static const scanvideo_mode_t bogus_mode = {
    .default_timing = &bogus_timing,
    .pio_program = &bogus_pio,
    .width = DISPLAYWIDTH,
    .height = DISPLAYHEIGHT,
    .xscale = 1,
    .yscale = 1,
};

#define FRAME_PERIOD 6600

extern SDL_Window *window;
static void display_driver_init() {

    scanvideo_setup(&bogus_mode);
}

static void simulate_display(uint dither) {
    static bool first = true;
    if (texture_raw && first) {
        SDL_SetWindowSize(window, 640, 360);
        first = false;
    }

    if (pico_access_surface) {
        uint8_t* data = (uint8_t*)pico_access_surface->pixels;

        uint w = MIN(MIN(DISPLAYWIDTH, SCREENWIDTH), pico_access_surface->w);
        uint h = MIN(MIN(DISPLAYHEIGHT, SCREENHEIGHT), pico_access_surface->h);
        
        for (int y = 0; y < h; ++y) {
            uint16_t* row = (uint16_t*)((uint8_t*)pico_access_surface->pixels + pico_access_surface->pitch * y);
            dither ^= 1;
            for (int x = 0; x < w; ++x) {
                dither ^= 1;

#if FSAA
                uint8_t *pframe = &frame_buffer[display_frame_index][y*(SCREENWIDTH<<FSAA) + (x<<FSAA)];
                uint lum = 0;
                for (int aay=0; aay<(1<<FSAA); ++aay) {
                    for (int aax=0; aax<(1<<FSAA); ++aax) {
                        lum += display_palette[pframe[aay*SCREENWIDTH + aax]];
                    }
                }
                lum >>= (FSAA*2);
                pframe += (1<<FSAA);
#else
                uint8_t *pframe = &frame_buffer[display_frame_index][y*SCREENWIDTH + x];
                uint lum = display_palette[*pframe];
#endif

                lum = (lum >> 5) + ((lum >> 4) & dither);
                lum = MIN(lum, 7);

#if DEBUGLINE
                if (x < 6)
                    lum = (-(((y>>(5-x))&1)==0))&7;
                if (y == debugline)
                    lum = 7;
#endif
                lum *= 36;
                row[x] = PICO_SCANVIDEO_PIXEL_FROM_RGB8(lum, lum, lum);
            }
        }
    }

    send_update_screen();
    SDL_Delay(1);
}


#endif

//#define TESTCARD_BAR 1

static void core1() {
    absolute_time_t frame_time = get_absolute_time();

    uint l = 0;
    uint dither = 0;

    while (true) {
#if defined J_OLED_MONO

#if JTBD16

#if JTBD16_SHADOW_GAMMA == 0
        // pow(0.50) — aggressive shadow lift, bp=12, wp=232
        static const uint8_t remap_lut[256] = {
              0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,  17,  24,  30,
             34,  38,  42,  45,  49,  52,  54,  57,  60,  62,  64,  67,  69,  71,  73,  75,
             77,  79,  81,  82,  84,  86,  88,  89,  91,  93,  94,  96,  97,  99, 100, 102,
            103, 105, 106, 107, 109, 110, 111, 113, 114, 115, 117, 118, 119, 120, 122, 123,
            124, 125, 126, 128, 129, 130, 131, 132, 133, 134, 135, 136, 138, 139, 140, 141,
            142, 143, 144, 145, 146, 147, 148, 149, 150, 151, 152, 153, 154, 155, 156, 157,
            158, 159, 159, 160, 161, 162, 163, 164, 165, 166, 167, 168, 168, 169, 170, 171,
            172, 173, 174, 174, 175, 176, 177, 178, 179, 179, 180, 181, 182, 183, 184, 184,
            185, 186, 187, 188, 188, 189, 190, 191, 191, 192, 193, 194, 195, 195, 196, 197,
            198, 198, 199, 200, 200, 201, 202, 203, 203, 204, 205, 206, 206, 207, 208, 208,
            209, 210, 211, 211, 212, 213, 213, 214, 215, 215, 216, 217, 217, 218, 219, 219,
            220, 221, 222, 222, 223, 223, 224, 225, 225, 226, 227, 227, 228, 229, 229, 230,
            231, 231, 232, 233, 233, 234, 234, 235, 236, 236, 237, 238, 238, 239, 239, 240,
            241, 241, 242, 243, 243, 244, 244, 245, 246, 246, 247, 247, 248, 249, 249, 250,
            250, 251, 251, 252, 253, 253, 254, 254, 255, 255, 255, 255, 255, 255, 255, 255,
            255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255,
        };
#elif JTBD16_SHADOW_GAMMA == 1
        // pow(0.625) — moderate shadow lift, bp=12, wp=232
        static const uint8_t remap_lut[256] = {
              0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   9,  14,  17,
             21,  24,  27,  30,  32,  35,  37,  39,  41,  44,  46,  48,  50,  51,  53,  55,
             57,  59,  60,  62,  64,  65,  67,  69,  70,  72,  73,  75,  76,  78,  79,  81,
             82,  84,  85,  86,  88,  89,  91,  92,  93,  95,  96,  97,  98, 100, 101, 102,
            104, 105, 106, 107, 108, 110, 111, 112, 113, 114, 116, 117, 118, 119, 120, 121,
            122, 124, 125, 126, 127, 128, 129, 130, 131, 132, 133, 134, 136, 137, 138, 139,
            140, 141, 142, 143, 144, 145, 146, 147, 148, 149, 150, 151, 152, 153, 154, 155,
            156, 157, 158, 159, 160, 161, 162, 163, 163, 164, 165, 166, 167, 168, 169, 170,
            171, 172, 173, 174, 175, 175, 176, 177, 178, 179, 180, 181, 182, 183, 184, 184,
            185, 186, 187, 188, 189, 190, 191, 191, 192, 193, 194, 195, 196, 197, 197, 198,
            199, 200, 201, 202, 202, 203, 204, 205, 206, 207, 207, 208, 209, 210, 211, 211,
            212, 213, 214, 215, 215, 216, 217, 218, 219, 219, 220, 221, 222, 223, 223, 224,
            225, 226, 227, 227, 228, 229, 230, 230, 231, 232, 233, 233, 234, 235, 236, 236,
            237, 238, 239, 240, 240, 241, 242, 242, 243, 244, 245, 245, 246, 247, 248, 248,
            249, 250, 251, 251, 252, 253, 254, 254, 255, 255, 255, 255, 255, 255, 255, 255,
            255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255,
        };
#elif JTBD16_SHADOW_GAMMA == 2
        // pow(0.80) — mild shadow lift, bp=12, wp=232
        static const uint8_t remap_lut[256] = {
              0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   3,   6,   8,
             10,  12,  14,  16,  18,  20,  22,  23,  25,  27,  28,  30,  31,  33,  34,  36,
             37,  39,  40,  42,  43,  45,  46,  48,  49,  50,  52,  53,  55,  56,  57,  59,
             60,  61,  63,  64,  65,  67,  68,  69,  70,  72,  73,  74,  75,  77,  78,  79,
             80,  82,  83,  84,  85,  87,  88,  89,  90,  91,  93,  94,  95,  96,  97,  99,
            100, 101, 102, 103, 104, 106, 107, 108, 109, 110, 111, 112, 114, 115, 116, 117,
            118, 119, 120, 121, 123, 124, 125, 126, 127, 128, 129, 130, 131, 132, 134, 135,
            136, 137, 138, 139, 140, 141, 142, 143, 144, 145, 146, 148, 149, 150, 151, 152,
            153, 154, 155, 156, 157, 158, 159, 160, 161, 162, 163, 164, 165, 166, 167, 168,
            169, 170, 172, 173, 174, 175, 176, 177, 178, 179, 180, 181, 182, 183, 184, 185,
            186, 187, 188, 189, 190, 191, 192, 193, 194, 195, 196, 197, 198, 199, 200, 201,
            202, 203, 204, 205, 206, 206, 207, 208, 209, 210, 211, 212, 213, 214, 215, 216,
            217, 218, 219, 220, 221, 222, 223, 224, 225, 226, 227, 228, 229, 230, 231, 232,
            232, 233, 234, 235, 236, 237, 238, 239, 240, 241, 242, 243, 244, 245, 246, 247,
            248, 248, 249, 250, 251, 252, 253, 254, 255, 255, 255, 255, 255, 255, 255, 255,
            255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255,
        };
#elif JTBD16_SHADOW_GAMMA == 3
        // linear — black/white point only, bp=12, wp=232
        static const uint8_t remap_lut[256] = {
              0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   1,   2,   3,
              5,   6,   7,   8,   9,  10,  12,  13,  14,  15,  16,  17,  19,  20,  21,  22,
             23,  24,  26,  27,  28,  29,  30,  31,  32,  34,  35,  36,  37,  38,  39,  41,
             42,  43,  44,  45,  46,  48,  49,  50,  51,  52,  53,  54,  56,  57,  58,  59,
             60,  61,  63,  64,  65,  66,  67,  68,  70,  71,  72,  73,  74,  75,  77,  78,
             79,  80,  81,  82,  83,  85,  86,  87,  88,  89,  90,  92,  93,  94,  95,  96,
             97,  99, 100, 101, 102, 103, 104, 105, 107, 108, 109, 110, 111, 112, 114, 115,
            116, 117, 118, 119, 121, 122, 123, 124, 125, 126, 128, 129, 130, 131, 132, 133,
            134, 136, 137, 138, 139, 140, 141, 143, 144, 145, 146, 147, 148, 150, 151, 152,
            153, 154, 155, 156, 158, 159, 160, 161, 162, 163, 165, 166, 167, 168, 169, 170,
            172, 173, 174, 175, 176, 177, 179, 180, 181, 182, 183, 184, 185, 187, 188, 189,
            190, 191, 192, 194, 195, 196, 197, 198, 199, 201, 202, 203, 204, 205, 206, 207,
            209, 210, 211, 212, 213, 214, 216, 217, 218, 219, 220, 221, 223, 224, 225, 226,
            227, 228, 230, 231, 232, 233, 234, 235, 236, 238, 239, 240, 241, 242, 243, 245,
            246, 247, 248, 249, 250, 252, 253, 254, 255, 255, 255, 255, 255, 255, 255, 255,
            255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255,
        };
#else
#error "JTBD16_SHADOW_GAMMA must be 0-3"
#endif

        // Blue noise texture — shared by modes 1, 2, 4
        static const uint8_t blue_noise[16][16] = {
            {114,   5,  74, 207,  37, 222,  71,   2,  83,  21, 225, 135,  24, 152, 199, 179},
            { 63, 194, 136,  15,  90, 192, 138, 162, 205, 180,  26,  27, 214,  97, 131,  35},
            {164, 224, 106, 247, 153, 120,  48, 243, 111,  13, 251, 163,   4, 232,  78, 253},
            { 29,  84,  46, 176,  59, 210,  30,  96,  62, 146, 124,  91, 184, 118,  43, 142},
            {183, 216, 129,   8, 233,  81, 169, 226, 187, 218,  75, 206,  54, 151, 209, 102},
            { 52, 155,  73, 191, 143, 108,  42, 132,   6,  22, 172,  38, 242,  69,  11, 230},
            { 92,  33, 244,  99,  16, 203, 240,  67, 157, 248, 103, 139, 115, 190, 165, 122},
            {201, 173,  58, 223, 161,  55, 182, 119,  94, 198,  51, 228,   1,  85,  19, 249},
            { 39, 141, 116,   0, 128,  86,  20, 221,  10, 145,  79, 168, 200, 220, 133,  65},
            {195, 236,  76, 213, 188, 254, 148, 174,  60, 235, 125,  34,  98,  44, 160, 105},
            { 12,  95, 159,  40, 100,  23,  72, 112, 211,  41, 186, 255, 150,  57, 238, 181},
            { 25, 217,  53, 177, 134, 227, 202,  14,  89, 158,  70,  28, 110, 215,  82, 126},
            {252, 144, 109, 237,  66,   7, 166, 140, 245, 117, 219, 137, 197,  36, 171,   3},
            {185,  68, 204,  32, 196, 123,  80, 189,  50,  31, 178,  45,  77, 231, 147, 101},
            { 17, 127,  47,  88, 149, 250,  18, 104, 208,  64, 239,  93, 167, 121,  61, 212},
            {229, 156, 241, 170, 113,  56, 175, 234, 130, 154, 107, 193,   9, 246,  49,  87},
        };

        // Combined LUT: maps palette index → gamma-corrected luminance in one lookup
        // instead of two (display_palette[] then remap_lut[]). Rebuilt per frame.
        static uint8_t combined_lut[256];

        // DMA-compatible frame buffer: each uint32_t holds (byte << 24)
        // for direct DMA to PIO TX FIFO (MSB-first autopull).
        static uint32_t dma_buf[DISPLAYWIDTH * (DISPLAYHEIGHT / 8)];

        // Helper: rebuild combined LUT from current palette + gamma remap.
        // Only 256 iterations — trivial vs 8192 pixels, saves one lookup per pixel.
        #define REBUILD_COMBINED_LUT() do { \
            for (int _i = 0; _i < 256; _i++) \
                combined_lut[_i] = remap_lut[display_palette[_i]]; \
        } while (0)

        // Helper: pack frame[] into dma_buf[] for DMA transfer
        #define PACK_DMA_BUF(frame) do { \
            for (int _i = 0; _i < DISPLAYWIDTH * (DISPLAYHEIGHT / 8); _i++) \
                dma_buf[_i] = (uint32_t)(frame)[_i] << 24; \
        } while (0)

        // Helper: send frame via DMA (non-blocking SPI transfer)
        #define DMA_SEND_FRAME() do { \
            gpio_put(J_OLED_CS, 0); \
            gpio_put(J_OLED_DC, 0); \
            oled_spi_write_blocking(command_park, sizeof(command_park)); \
            gpio_put(J_OLED_DC, 1); \
            oled_spi_dma_start(dma_buf, DISPLAYWIDTH * (DISPLAYHEIGHT / 8)); \
            oled_spi_dma_wait(); \
            gpio_put(J_OLED_CS, 1); \
        } while (0)

        // Helper macro: single-lookup luminance via combined LUT
        // (replaces old 2-lookup LUM_AT: remap_lut[display_palette[...]])
        #define LUM_AT(fb, y, x) combined_lut[(fb)[(y) * SCREENWIDTH + (x)]]

#if JTBD16_PREDITHER_SMOOTH
        // Pre-dither 3×3 Gaussian low-pass filter to reduce moiré from
        // texture aliasing at 128×64. Kernel: [1 2 1; 2 4 2; 1 2 1] / 16.
        // Costs ~1 ms per frame at 150 MHz — well within the 16.7 ms budget.
        static uint8_t raw_lum_buf[DISPLAYHEIGHT * DISPLAYWIDTH];
        static uint8_t smooth_lum[DISPLAYHEIGHT * DISPLAYWIDTH];

        #define BUILD_SMOOTH_LUM(fb) do { \
            for (int _y = 0; _y < DISPLAYHEIGHT; _y++) \
                for (int _x = 0; _x < DISPLAYWIDTH; _x++) \
                    raw_lum_buf[_y * DISPLAYWIDTH + _x] = \
                        combined_lut[(fb)[_y * SCREENWIDTH + _x]]; \
            for (int _y = 0; _y < DISPLAYHEIGHT; _y++) { \
                const int _y0 = (_y > 0) ? _y - 1 : 0; \
                const int _y2 = (_y < DISPLAYHEIGHT - 1) ? _y + 1 : _y; \
                const uint8_t *_r0 = &raw_lum_buf[_y0 * DISPLAYWIDTH]; \
                const uint8_t *_r1 = &raw_lum_buf[_y  * DISPLAYWIDTH]; \
                const uint8_t *_r2 = &raw_lum_buf[_y2 * DISPLAYWIDTH]; \
                for (int _x = 0; _x < DISPLAYWIDTH; _x++) { \
                    const int _x0 = (_x > 0) ? _x - 1 : 0; \
                    const int _x2 = (_x < DISPLAYWIDTH - 1) ? _x + 1 : _x; \
                    smooth_lum[_y * DISPLAYWIDTH + _x] = ( \
                        _r0[_x0] + _r0[_x] * 2 + _r0[_x2] + \
                        _r1[_x0] * 2 + _r1[_x] * 4 + _r1[_x2] * 2 + \
                        _r2[_x0] + _r2[_x] * 2 + _r2[_x2] \
                    ) >> 4; \
                } \
            } \
        } while (0)

        #undef LUM_AT
        #define LUM_AT(fb, y, x) smooth_lum[(y) * DISPLAYWIDTH + (x)]
#else
        #define BUILD_SMOOTH_LUM(fb) ((void)0)
#endif

#if JTBD16_DITHER_MODE == DITHER_ATKINSON
        // =========================================================
        // Mode 0: Atkinson error-diffusion dithering
        // =========================================================
        {
            static uint8_t frame[DISPLAYWIDTH * (DISPLAYHEIGHT / 8)];
            static int16_t err_buf[3][DISPLAYWIDTH];

            sem_acquire_blocking(&vsync);

            while (true) {
                uint8_t last_fi = display_frame_index;
                uint8_t *fb = frame_buffer[display_frame_index];

                REBUILD_COMBINED_LUT();
                BUILD_SMOOTH_LUM(fb);
                memset(frame, 0, sizeof(frame));
                memset(err_buf, 0, sizeof(err_buf));

                for (int sy = 0; sy < DISPLAYHEIGHT; sy++) {
                    int page = sy >> 3;
                    int bit  = sy & 7;
                    int er   = sy % 3;

                    for (int sx = 0; sx < DISPLAYWIDTH; sx++) {
                        int lum = LUM_AT(fb, sy, sx);
                        int val = lum + err_buf[er][sx];
                        int out = (val > JTBD16_DITHER_THRESHOLD) ? 255 : 0;
                        int qe  = (val - out) >> 3;

                        if (sx + 1 < DISPLAYWIDTH) err_buf[er][sx + 1] += qe;
                        if (sx + 2 < DISPLAYWIDTH) err_buf[er][sx + 2] += qe;
                        if (sy + 1 < DISPLAYHEIGHT) {
                            int r1 = (sy + 1) % 3;
                            if (sx > 0)                err_buf[r1][sx - 1] += qe;
                                                       err_buf[r1][sx]     += qe;
                            if (sx + 1 < DISPLAYWIDTH) err_buf[r1][sx + 1] += qe;
                        }
                        if (sy + 2 < DISPLAYHEIGHT) {
                            err_buf[(sy + 2) % 3][sx] += qe;
                        }

                        if (out) {
                            frame[page * DISPLAYWIDTH + sx] |= (1 << bit);
                        }
                    }

                    memset(err_buf[er], 0, DISPLAYWIDTH * sizeof(int16_t));
                }

                PACK_DMA_BUF(frame);
                sem_release(&vsync);

                do {
                    DMA_SEND_FRAME();
                    __dmb();
                } while (*(volatile uint8_t *)&display_frame_index == last_fi);

                sem_acquire_blocking(&vsync);
            }
        }

#elif JTBD16_DITHER_MODE == DITHER_BLUENOISE_STATIC
        // =========================================================
        // Mode 1: Static blue noise ordered dithering
        // =========================================================
        {
            static uint8_t frame[DISPLAYWIDTH * (DISPLAYHEIGHT / 8)];

            sem_acquire_blocking(&vsync);

            while (true) {
                uint8_t last_fi = display_frame_index;
                uint8_t *fb = frame_buffer[display_frame_index];

                REBUILD_COMBINED_LUT();
                BUILD_SMOOTH_LUM(fb);

                for (int p = 0; p < (DISPLAYHEIGHT / 8); p++) {
                    for (int x = 0; x < DISPLAYWIDTH; x++) {
                        uint8_t col = 0;
                        for (int b = 0; b < 8; b++) {
                            int y = p * 8 + b;
                            uint lum = LUM_AT(fb, y, x);
                            col >>= 1;
                            if (lum > blue_noise[y & 15][x & 15]) col |= 0x80;
                        }
                        frame[p * DISPLAYWIDTH + x] = col;
                    }
                }

                PACK_DMA_BUF(frame);
                sem_release(&vsync);

                do {
                    DMA_SEND_FRAME();
                    __dmb();
                } while (*(volatile uint8_t *)&display_frame_index == last_fi);

                sem_acquire_blocking(&vsync);
            }
        }

#elif JTBD16_DITHER_MODE == DITHER_BLUENOISE_TEMPORAL
        // =========================================================
        // Mode 2: 4-frame phase-shifted blue noise dithering
        // =========================================================
        {
            static const uint8_t phase_dx[4] = {0, 7, 3, 11};
            static const uint8_t phase_dy[4] = {0, 3, 11,  7};

            static uint8_t frames[4][DISPLAYWIDTH * (DISPLAYHEIGHT / 8)];

            sem_acquire_blocking(&vsync);

            while (true) {
                uint8_t last_fi = display_frame_index;
                uint8_t *fb = frame_buffer[display_frame_index];

                REBUILD_COMBINED_LUT();
                BUILD_SMOOTH_LUM(fb);

                for (int p = 0; p < (DISPLAYHEIGHT / 8); p++) {
                    for (int x = 0; x < DISPLAYWIDTH; x++) {
                        uint8_t c0 = 0, c1 = 0, c2 = 0, c3 = 0;
                        for (int b = 0; b < 8; b++) {
                            int y = p * 8 + b;
                            uint lum = LUM_AT(fb, y, x);
                            c0 >>= 1; c1 >>= 1; c2 >>= 1; c3 >>= 1;
                            if (lum > blue_noise[(y             ) & 15][(x             ) & 15]) c0 |= 0x80;
                            if (lum > blue_noise[(y + phase_dy[1]) & 15][(x + phase_dx[1]) & 15]) c1 |= 0x80;
                            if (lum > blue_noise[(y + phase_dy[2]) & 15][(x + phase_dx[2]) & 15]) c2 |= 0x80;
                            if (lum > blue_noise[(y + phase_dy[3]) & 15][(x + phase_dx[3]) & 15]) c3 |= 0x80;
                        }
                        int idx = p * DISPLAYWIDTH + x;
                        frames[0][idx] = c0;
                        frames[1][idx] = c1;
                        frames[2][idx] = c2;
                        frames[3][idx] = c3;
                    }
                }

                sem_release(&vsync);

                do {
                    for (int f = 0; f < 4; f++) {
                        PACK_DMA_BUF(frames[f]);
                        DMA_SEND_FRAME();
                    }
                    __dmb();
                } while (*(volatile uint8_t *)&display_frame_index == last_fi);

                sem_acquire_blocking(&vsync);
            }
        }

#elif JTBD16_DITHER_MODE == DITHER_3PASS_CONTRAST
        // =========================================================
        // Mode 3: 3-pass contrast-weighted greyscale
        // =========================================================
        // Original rp2040-doom technique. ~7 grey levels.
        // Known to flicker on SSD1309 128x64.
        {
            sem_acquire_blocking(&vsync);

            while (true) {
                uint8_t last_fi = display_frame_index;

                REBUILD_COMBINED_LUT();

                for (uint pass = 0; pass < 3; pass++) {
                    uint8_t level = 0x04 >> pass;
                    uint d = dither;

                    for (int p = 0; p < (DISPLAYHEIGHT / 8); ++p) {
                        for (int x = 0; x < DISPLAYWIDTH; ++x) {
                            d ^= 1;
                            uint8_t byte = 0;
                            for (int b = 0; b < 8; ++b) {
                                d ^= 1;
                                int y = p * 8 + b;
                                uint lum = combined_lut[frame_buffer[display_frame_index][y * SCREENWIDTH + x]];
                                lum = (lum >> 5) + ((lum >> 4) & d);
                                if (lum > 7) lum = 7;
                                byte >>= 1;
                                if (lum & level) {
                                    byte |= 0x80;
                                }
                            }
                            field_buffer[p * DISPLAYWIDTH + x] = byte;
                        }
                    }

                    command_run[1] = contrast[pass];

                    gpio_put(J_OLED_CS, 0);
                    gpio_put(J_OLED_DC, 0);
                    oled_spi_write_blocking(command_park, sizeof(command_park));
                    gpio_put(J_OLED_DC, 1);
                    oled_spi_write_blocking(field_buffer, sizeof(field_buffer));
                    gpio_put(J_OLED_DC, 0);
                    oled_spi_write_blocking(command_run, sizeof(command_run));
                    gpio_put(J_OLED_CS, 1);
                }

                dither ^= 1;
                sem_release(&vsync);

                frame_time = delayed_by_us(frame_time, FRAME_PERIOD);
                sleep_until(frame_time);

                __dmb();
                if (*(volatile uint8_t *)&display_frame_index != last_fi) {
                    // New frame available
                }

                sem_acquire_blocking(&vsync);
            }
        }

#elif JTBD16_DITHER_MODE == DITHER_BLUENOISE_EDGE
        // =========================================================
        // Mode 4: Blue noise + edge boost (unsharp mask)
        // =========================================================
        {
            static uint8_t frame[DISPLAYWIDTH * (DISPLAYHEIGHT / 8)];
            static uint8_t lum_prev[DISPLAYWIDTH];
            static uint8_t lum_curr[DISPLAYWIDTH];

            sem_acquire_blocking(&vsync);

            while (true) {
                uint8_t last_fi = display_frame_index;
                uint8_t *fb = frame_buffer[display_frame_index];

                REBUILD_COMBINED_LUT();
                BUILD_SMOOTH_LUM(fb);
                memset(frame, 0, sizeof(frame));

                // Pre-fill first row
                {
                    for (int x = 0; x < DISPLAYWIDTH; x++)
                        lum_prev[x] = LUM_AT(fb, 0, x);
                }

                for (int sy = 0; sy < DISPLAYHEIGHT; sy++) {
                    int page = sy >> 3;
                    int bit  = sy & 7;

                    for (int x = 0; x < DISPLAYWIDTH; x++)
                        lum_curr[x] = LUM_AT(fb, sy, x);

                    int next_y = (sy + 1 < DISPLAYHEIGHT) ? sy + 1 : sy;

                    for (int x = 0; x < DISPLAYWIDTH; x++) {
                        int c = lum_curr[x];
                        int left  = (x > 0) ? lum_curr[x - 1] : c;
                        int right = (x + 1 < DISPLAYWIDTH) ? lum_curr[x + 1] : c;
                        int up    = lum_prev[x];
                        int down  = LUM_AT(fb, next_y, x);

                        int mean = (c * 4 + left + right + up + down) >> 3;
                        int boosted = c + (((c - mean) * JTBD16_EDGE_STRENGTH) >> 7);
                        if (boosted < 0) boosted = 0;
                        if (boosted > 255) boosted = 255;

                        if (boosted > blue_noise[sy & 15][x & 15]) {
                            frame[page * DISPLAYWIDTH + x] |= (1 << bit);
                        }
                    }

                    memcpy(lum_prev, lum_curr, DISPLAYWIDTH);
                }

                PACK_DMA_BUF(frame);
                sem_release(&vsync);

                do {
                    DMA_SEND_FRAME();
                    __dmb();
                } while (*(volatile uint8_t *)&display_frame_index == last_fi);

                sem_acquire_blocking(&vsync);
            }
        }

#elif JTBD16_DITHER_MODE == DITHER_HYBRID_HUD
        // =========================================================
        // Mode 5: Hybrid Atkinson viewport + hard-threshold HUD
        // =========================================================
        {
            static uint8_t frame[DISPLAYWIDTH * (DISPLAYHEIGHT / 8)];
            static int16_t err_buf[3][DISPLAYWIDTH];

            sem_acquire_blocking(&vsync);

            while (true) {
                uint8_t last_fi = display_frame_index;
                uint8_t *fb = frame_buffer[display_frame_index];

                REBUILD_COMBINED_LUT();
                BUILD_SMOOTH_LUM(fb);
                memset(frame, 0, sizeof(frame));
                memset(err_buf, 0, sizeof(err_buf));

                for (int sy = 0; sy < DISPLAYHEIGHT; sy++) {
                    int page = sy >> 3;
                    int bit  = sy & 7;

                    if (sy < JTBD16_HUD_Y_START) {
                        // --- Atkinson region (3D viewport) ---
                        int er = sy % 3;
                        for (int sx = 0; sx < DISPLAYWIDTH; sx++) {
                            int lum = LUM_AT(fb, sy, sx);
                            int val = lum + err_buf[er][sx];
                            int out = (val > JTBD16_DITHER_THRESHOLD) ? 255 : 0;
                            int qe  = (val - out) >> 3;

                            if (sx + 1 < DISPLAYWIDTH) err_buf[er][sx + 1] += qe;
                            if (sx + 2 < DISPLAYWIDTH) err_buf[er][sx + 2] += qe;
                            if (sy + 1 < DISPLAYHEIGHT && sy + 1 < JTBD16_HUD_Y_START) {
                                int r1 = (sy + 1) % 3;
                                if (sx > 0)                err_buf[r1][sx - 1] += qe;
                                                           err_buf[r1][sx]     += qe;
                                if (sx + 1 < DISPLAYWIDTH) err_buf[r1][sx + 1] += qe;
                            }
                            if (sy + 2 < DISPLAYHEIGHT && sy + 2 < JTBD16_HUD_Y_START) {
                                err_buf[(sy + 2) % 3][sx] += qe;
                            }

                            if (out) {
                                frame[page * DISPLAYWIDTH + sx] |= (1 << bit);
                            }
                        }
                        memset(err_buf[er], 0, DISPLAYWIDTH * sizeof(int16_t));
                    } else {
                        // --- Hard threshold region (HUD/status bar) ---
                        for (int sx = 0; sx < DISPLAYWIDTH; sx++) {
                            int lum = LUM_AT(fb, sy, sx);
                            if (lum > JTBD16_HUD_THRESHOLD) {
                                frame[page * DISPLAYWIDTH + sx] |= (1 << bit);
                            }
                        }
                    }
                }

                PACK_DMA_BUF(frame);
                sem_release(&vsync);

                do {
                    DMA_SEND_FRAME();
                    __dmb();
                } while (*(volatile uint8_t *)&display_frame_index == last_fi);

                sem_acquire_blocking(&vsync);
            }
        }

#elif JTBD16_DITHER_MODE == DITHER_FLOYD_STEINBERG
        // =========================================================
        // Mode 6: Floyd-Steinberg error-diffusion dithering
        // =========================================================
        // Classic full error diffusion: distributes 100% of quantization
        // error to 4 neighbours (7/16, 3/16, 5/16, 1/16).
        // More grey levels than Atkinson but can produce "wormy" patterns.
        {
            static uint8_t frame[DISPLAYWIDTH * (DISPLAYHEIGHT / 8)];
            static int16_t err_curr[DISPLAYWIDTH + 2];
            static int16_t err_next[DISPLAYWIDTH + 2];

            sem_acquire_blocking(&vsync);

            while (true) {
                uint8_t last_fi = display_frame_index;
                uint8_t *fb = frame_buffer[display_frame_index];

                REBUILD_COMBINED_LUT();
                BUILD_SMOOTH_LUM(fb);
                memset(frame, 0, sizeof(frame));
                memset(err_curr, 0, sizeof(err_curr));
                memset(err_next, 0, sizeof(err_next));

                for (int sy = 0; sy < DISPLAYHEIGHT; sy++) {
                    int page = sy >> 3;
                    int bit  = sy & 7;

                    for (int sx = 0; sx < DISPLAYWIDTH; sx++) {
                        int lum = LUM_AT(fb, sy, sx);
                        int val = lum + err_curr[sx + 1];
                        int out = (val > JTBD16_DITHER_THRESHOLD) ? 255 : 0;
                        int qe  = val - out;

                        // Floyd-Steinberg error distribution (7/16, 3/16, 5/16, 1/16)
                        err_curr[sx + 2] += (qe * 7) >> 4;
                        err_next[sx]     += (qe * 3) >> 4;
                        err_next[sx + 1] += (qe * 5) >> 4;
                        err_next[sx + 2] += (qe * 1) >> 4;

                        if (out) {
                            frame[page * DISPLAYWIDTH + sx] |= (1 << bit);
                        }
                    }

                    memcpy(err_curr, err_next, sizeof(err_curr));
                    memset(err_next, 0, sizeof(err_next));
                }

                PACK_DMA_BUF(frame);
                sem_release(&vsync);

                do {
                    DMA_SEND_FRAME();
                    __dmb();
                } while (*(volatile uint8_t *)&display_frame_index == last_fi);

                sem_acquire_blocking(&vsync);
            }
        }

#elif JTBD16_DITHER_MODE == DITHER_SIERRA_LITE
        // =========================================================
        // Mode 7: Sierra Lite (two-row) error-diffusion dithering
        // =========================================================
        // Simplified Sierra: distributes error to only 3 neighbours
        // (2/4 right, 1/4 below-left, 1/4 below). Faster than FS,
        // nearly the same quality, less directional bias.
        {
            static uint8_t frame[DISPLAYWIDTH * (DISPLAYHEIGHT / 8)];
            static int16_t err_curr[DISPLAYWIDTH + 2];
            static int16_t err_next[DISPLAYWIDTH + 2];

            sem_acquire_blocking(&vsync);

            while (true) {
                uint8_t last_fi = display_frame_index;
                uint8_t *fb = frame_buffer[display_frame_index];

                REBUILD_COMBINED_LUT();
                BUILD_SMOOTH_LUM(fb);
                memset(frame, 0, sizeof(frame));
                memset(err_curr, 0, sizeof(err_curr));
                memset(err_next, 0, sizeof(err_next));

                for (int sy = 0; sy < DISPLAYHEIGHT; sy++) {
                    int page = sy >> 3;
                    int bit  = sy & 7;

                    for (int sx = 0; sx < DISPLAYWIDTH; sx++) {
                        int lum = LUM_AT(fb, sy, sx);
                        int val = lum + err_curr[sx + 1];
                        int out = (val > JTBD16_DITHER_THRESHOLD) ? 255 : 0;
                        int qe  = val - out;

                        // Sierra Lite: 2/4 right, 1/4 below-left, 1/4 below
                        err_curr[sx + 2] += (qe * 2) >> 2;
                        err_next[sx]     += (qe * 1) >> 2;
                        err_next[sx + 1] += (qe * 1) >> 2;

                        if (out) {
                            frame[page * DISPLAYWIDTH + sx] |= (1 << bit);
                        }
                    }

                    memcpy(err_curr, err_next, sizeof(err_curr));
                    memset(err_next, 0, sizeof(err_next));
                }

                PACK_DMA_BUF(frame);
                sem_release(&vsync);

                do {
                    DMA_SEND_FRAME();
                    __dmb();
                } while (*(volatile uint8_t *)&display_frame_index == last_fi);

                sem_acquire_blocking(&vsync);
            }
        }

#elif JTBD16_DITHER_MODE == DITHER_BN_FLOYD_STEINBERG
        // =========================================================
        // Mode 8: Blue-noise-modulated Floyd-Steinberg
        // =========================================================
        // The "best of both worlds" hybrid: Floyd-Steinberg error diffusion
        // with the threshold perturbed by blue noise. This breaks up the
        // regular "wormy" patterns of pure FS while keeping its superior
        // grey level reproduction. JTBD16_BN_MODULATION controls the
        // perturbation amplitude.
        {
            static uint8_t frame[DISPLAYWIDTH * (DISPLAYHEIGHT / 8)];
            static int16_t err_curr[DISPLAYWIDTH + 2];
            static int16_t err_next[DISPLAYWIDTH + 2];

            sem_acquire_blocking(&vsync);

            while (true) {
                uint8_t last_fi = display_frame_index;
                uint8_t *fb = frame_buffer[display_frame_index];

                REBUILD_COMBINED_LUT();
                BUILD_SMOOTH_LUM(fb);
                memset(frame, 0, sizeof(frame));
                memset(err_curr, 0, sizeof(err_curr));
                memset(err_next, 0, sizeof(err_next));

                for (int sy = 0; sy < DISPLAYHEIGHT; sy++) {
                    int page = sy >> 3;
                    int bit  = sy & 7;

                    for (int sx = 0; sx < DISPLAYWIDTH; sx++) {
                        int lum = LUM_AT(fb, sy, sx);
                        int val = lum + err_curr[sx + 1];

                        // Blue noise perturbation: shift threshold by ±BN_MODULATION/2
                        int bn = blue_noise[sy & 15][sx & 15];
                        int thr = JTBD16_DITHER_THRESHOLD
                                  + ((bn * JTBD16_BN_MODULATION) >> 8)
                                  - (JTBD16_BN_MODULATION >> 1);

                        int out = (val > thr) ? 255 : 0;
                        int qe  = val - out;

                        err_curr[sx + 2] += (qe * 7) >> 4;
                        err_next[sx]     += (qe * 3) >> 4;
                        err_next[sx + 1] += (qe * 5) >> 4;
                        err_next[sx + 2] += (qe * 1) >> 4;

                        if (out) {
                            frame[page * DISPLAYWIDTH + sx] |= (1 << bit);
                        }
                    }

                    memcpy(err_curr, err_next, sizeof(err_curr));
                    memset(err_next, 0, sizeof(err_next));
                }

                PACK_DMA_BUF(frame);
                sem_release(&vsync);

                do {
                    DMA_SEND_FRAME();
                    __dmb();
                } while (*(volatile uint8_t *)&display_frame_index == last_fi);

                sem_acquire_blocking(&vsync);
            }
        }

#elif JTBD16_DITHER_MODE == DITHER_BN_ATKINSON
        // =========================================================
        // Mode 9: Blue-noise-modulated Atkinson
        // =========================================================
        // Atkinson error diffusion with blue noise threshold perturbation.
        // Keeps Atkinson's clean surfaces and sharp edges while the BN
        // perturbation breaks up the "swimming" patterns in motion.
        // Should combine the best qualities of modes 0 and 1.
        {
            static uint8_t frame[DISPLAYWIDTH * (DISPLAYHEIGHT / 8)];
            static int16_t err_buf[3][DISPLAYWIDTH];

            sem_acquire_blocking(&vsync);

            while (true) {
                uint8_t last_fi = display_frame_index;
                uint8_t *fb = frame_buffer[display_frame_index];

                REBUILD_COMBINED_LUT();
                BUILD_SMOOTH_LUM(fb);
                memset(frame, 0, sizeof(frame));
                memset(err_buf, 0, sizeof(err_buf));

                for (int sy = 0; sy < DISPLAYHEIGHT; sy++) {
                    int page = sy >> 3;
                    int bit  = sy & 7;
                    int er   = sy % 3;

                    for (int sx = 0; sx < DISPLAYWIDTH; sx++) {
                        int lum = LUM_AT(fb, sy, sx);
                        int val = lum + err_buf[er][sx];

                        // Blue noise perturbation: shift threshold by ±BN_MODULATION/2
                        int bn = blue_noise[sy & 15][sx & 15];
                        int thr = JTBD16_DITHER_THRESHOLD
                                  + ((bn * JTBD16_BN_MODULATION) >> 8)
                                  - (JTBD16_BN_MODULATION >> 1);

                        int out = (val > thr) ? 255 : 0;
                        int qe  = (val - out) >> 3;  // Atkinson: distribute 6/8

                        if (sx + 1 < DISPLAYWIDTH) err_buf[er][sx + 1] += qe;
                        if (sx + 2 < DISPLAYWIDTH) err_buf[er][sx + 2] += qe;
                        if (sy + 1 < DISPLAYHEIGHT) {
                            int r1 = (sy + 1) % 3;
                            if (sx > 0)                err_buf[r1][sx - 1] += qe;
                                                       err_buf[r1][sx]     += qe;
                            if (sx + 1 < DISPLAYWIDTH) err_buf[r1][sx + 1] += qe;
                        }
                        if (sy + 2 < DISPLAYHEIGHT) {
                            err_buf[(sy + 2) % 3][sx] += qe;
                        }

                        if (out) {
                            frame[page * DISPLAYWIDTH + sx] |= (1 << bit);
                        }
                    }

                    memset(err_buf[er], 0, DISPLAYWIDTH * sizeof(int16_t));
                }

                PACK_DMA_BUF(frame);
                sem_release(&vsync);

                do {
                    DMA_SEND_FRAME();
                    __dmb();
                } while (*(volatile uint8_t *)&display_frame_index == last_fi);

                sem_acquire_blocking(&vsync);
            }
        }

#elif JTBD16_DITHER_MODE == DITHER_BAYER4X4
        // =========================================================
        // Mode 10: Bayer 4×4 ordered dithering
        // =========================================================
        // Classic ordered dithering using a 4×4 threshold matrix.
        // No error propagation — each pixel decided independently.
        // Very stable in motion (no cascading changes), but the 4×4
        // repeat pattern can produce a visible crosshatch grid.
        // Recommended by jborza: preferred over FS for real-time video
        // because small scene changes don't cascade across the whole frame.
        {
            static uint8_t frame[DISPLAYWIDTH * (DISPLAYHEIGHT / 8)];

            // Bayer 4×4 threshold matrix, scaled to 0-255
            // M(i,j) = (M4[i][j] + 0.5) * 256/16
            static const uint8_t bayer4[4][4] = {
                {  8, 136,  40, 168},
                {200,  72, 232, 104},
                { 56, 184,  24, 152},
                {248, 120, 216,  88},
            };

            sem_acquire_blocking(&vsync);

            while (true) {
                uint8_t last_fi = display_frame_index;
                uint8_t *fb = frame_buffer[display_frame_index];

                REBUILD_COMBINED_LUT();
                BUILD_SMOOTH_LUM(fb);

                for (int p = 0; p < (DISPLAYHEIGHT / 8); p++) {
                    for (int x = 0; x < DISPLAYWIDTH; x++) {
                        uint8_t col = 0;
                        for (int b = 0; b < 8; b++) {
                            int y = p * 8 + b;
                            uint lum = LUM_AT(fb, y, x);
                            col >>= 1;
                            if (lum > bayer4[y & 3][x & 3]) col |= 0x80;
                        }
                        frame[p * DISPLAYWIDTH + x] = col;
                    }
                }

                PACK_DMA_BUF(frame);
                sem_release(&vsync);

                do {
                    DMA_SEND_FRAME();
                    __dmb();
                } while (*(volatile uint8_t *)&display_frame_index == last_fi);

                sem_acquire_blocking(&vsync);
            }
        }

#elif JTBD16_DITHER_MODE == DITHER_BAYER8X8
        // =========================================================
        // Mode 11: Bayer 8×8 ordered dithering
        // =========================================================
        // Larger matrix produces a finer, less visible dither pattern
        // than 4×4 while retaining the same motion-stability benefits.
        // 64 threshold levels vs 16 for 4×4.
        {
            static uint8_t frame[DISPLAYWIDTH * (DISPLAYHEIGHT / 8)];

            // Bayer 8×8 threshold matrix, scaled to 0-255
            // M(i,j) = (M8[i][j] + 0.5) * 256/64
            static const uint8_t bayer8[8][8] = {
                {  2, 130,  34, 162,  10, 138,  42, 170},
                {194,  66, 226,  98, 202,  74, 234, 106},
                { 50, 178,  18, 146,  58, 186,  26, 154},
                {242, 114, 210,  82, 250, 122, 218,  90},
                { 14, 142,  46, 174,   6, 134,  38, 166},
                {206,  78, 238, 110, 198,  70, 230, 102},
                { 62, 190,  30, 158,  54, 182,  22, 150},
                {254, 126, 222,  94, 246, 118, 214,  86},
            };

            sem_acquire_blocking(&vsync);

            while (true) {
                uint8_t last_fi = display_frame_index;
                uint8_t *fb = frame_buffer[display_frame_index];

                REBUILD_COMBINED_LUT();
                BUILD_SMOOTH_LUM(fb);

                for (int p = 0; p < (DISPLAYHEIGHT / 8); p++) {
                    for (int x = 0; x < DISPLAYWIDTH; x++) {
                        uint8_t col = 0;
                        for (int b = 0; b < 8; b++) {
                            int y = p * 8 + b;
                            uint lum = LUM_AT(fb, y, x);
                            col >>= 1;
                            if (lum > bayer8[y & 7][x & 7]) col |= 0x80;
                        }
                        frame[p * DISPLAYWIDTH + x] = col;
                    }
                }

                PACK_DMA_BUF(frame);
                sem_release(&vsync);

                do {
                    DMA_SEND_FRAME();
                    __dmb();
                } while (*(volatile uint8_t *)&display_frame_index == last_fi);

                sem_acquire_blocking(&vsync);
            }
        }

#elif JTBD16_DITHER_MODE == DITHER_SERPENTINE_FS
        // =========================================================
        // Mode 12: Serpentine Floyd-Steinberg
        // =========================================================
        // Floyd-Steinberg with alternating left-to-right / right-to-left
        // scan direction each row. This breaks up the directional "wormy"
        // patterns that standard FS produces, especially in mid-tone areas.
        // Key technique from Tanner Helland's research.
        {
            static uint8_t frame[DISPLAYWIDTH * (DISPLAYHEIGHT / 8)];
            static int16_t err_curr[DISPLAYWIDTH + 2];
            static int16_t err_next[DISPLAYWIDTH + 2];

            sem_acquire_blocking(&vsync);

            while (true) {
                uint8_t last_fi = display_frame_index;
                uint8_t *fb = frame_buffer[display_frame_index];

                REBUILD_COMBINED_LUT();
                BUILD_SMOOTH_LUM(fb);
                memset(frame, 0, sizeof(frame));
                memset(err_curr, 0, sizeof(err_curr));
                memset(err_next, 0, sizeof(err_next));

                for (int sy = 0; sy < DISPLAYHEIGHT; sy++) {
                    int page = sy >> 3;
                    int bit  = sy & 7;
                    int left_to_right = !(sy & 1);

                    if (left_to_right) {
                        for (int sx = 0; sx < DISPLAYWIDTH; sx++) {
                            int lum = LUM_AT(fb, sy, sx);
                            int val = lum + err_curr[sx + 1];
                            int out = (val > JTBD16_DITHER_THRESHOLD) ? 255 : 0;
                            int qe  = val - out;

                            err_curr[sx + 2] += (qe * 7) >> 4;
                            err_next[sx]     += (qe * 3) >> 4;
                            err_next[sx + 1] += (qe * 5) >> 4;
                            err_next[sx + 2] += (qe * 1) >> 4;

                            if (out) {
                                frame[page * DISPLAYWIDTH + sx] |= (1 << bit);
                            }
                        }
                    } else {
                        // Right-to-left: mirror the FS kernel
                        for (int sx = DISPLAYWIDTH - 1; sx >= 0; sx--) {
                            int lum = LUM_AT(fb, sy, sx);
                            int val = lum + err_curr[sx + 1];
                            int out = (val > JTBD16_DITHER_THRESHOLD) ? 255 : 0;
                            int qe  = val - out;

                            err_curr[sx]     += (qe * 7) >> 4;  // left (reversed)
                            err_next[sx + 2] += (qe * 3) >> 4;  // below-right (reversed)
                            err_next[sx + 1] += (qe * 5) >> 4;  // below
                            err_next[sx]     += (qe * 1) >> 4;  // below-left (reversed)

                            if (out) {
                                frame[page * DISPLAYWIDTH + sx] |= (1 << bit);
                            }
                        }
                    }

                    memcpy(err_curr, err_next, sizeof(err_curr));
                    memset(err_next, 0, sizeof(err_next));
                }

                PACK_DMA_BUF(frame);
                sem_release(&vsync);

                do {
                    DMA_SEND_FRAME();
                    __dmb();
                } while (*(volatile uint8_t *)&display_frame_index == last_fi);

                sem_acquire_blocking(&vsync);
            }
        }

#elif JTBD16_DITHER_MODE == DITHER_JJN
        // =========================================================
        // Mode 13: Jarvis-Judice-Ninke error-diffusion dithering
        // =========================================================
        // 3-row, 12-coefficient kernel distributing error over a wider
        // neighbourhood than Floyd-Steinberg. Produces smoother results
        // with less visible directional patterning. Divides by 48.
        //
        //             X   7   5
        //     3   5   7   5   3
        //     1   3   5   3   1
        //
        // All weights sum to 48; uses >>4 then /3 decomposition for speed.
        {
            static uint8_t frame[DISPLAYWIDTH * (DISPLAYHEIGHT / 8)];
            static int16_t err_buf[3][DISPLAYWIDTH + 4]; // +4 for kernel spread

            sem_acquire_blocking(&vsync);

            while (true) {
                uint8_t last_fi = display_frame_index;
                uint8_t *fb = frame_buffer[display_frame_index];

                REBUILD_COMBINED_LUT();
                BUILD_SMOOTH_LUM(fb);
                memset(frame, 0, sizeof(frame));
                memset(err_buf, 0, sizeof(err_buf));

                for (int sy = 0; sy < DISPLAYHEIGHT; sy++) {
                    int page = sy >> 3;
                    int bit  = sy & 7;
                    int r0   = sy % 3;
                    int r1   = (sy + 1) % 3;
                    int r2   = (sy + 2) % 3;

                    for (int sx = 0; sx < DISPLAYWIDTH; sx++) {
                        int ex = sx + 2; // offset into err_buf (+2 for left kernel spread)
                        int lum = LUM_AT(fb, sy, sx);
                        int val = lum + err_buf[r0][ex];
                        int out = (val > JTBD16_DITHER_THRESHOLD) ? 255 : 0;
                        int qe  = val - out;

                        // JJN kernel: weights/48
                        // Row 0 (current):         X  7/48  5/48
                        err_buf[r0][ex + 1] += (qe * 7) / 48;
                        err_buf[r0][ex + 2] += (qe * 5) / 48;
                        // Row 1: 3/48  5/48  7/48  5/48  3/48
                        err_buf[r1][ex - 2] += (qe * 3) / 48;
                        err_buf[r1][ex - 1] += (qe * 5) / 48;
                        err_buf[r1][ex]     += (qe * 7) / 48;
                        err_buf[r1][ex + 1] += (qe * 5) / 48;
                        err_buf[r1][ex + 2] += (qe * 3) / 48;
                        // Row 2: 1/48  3/48  5/48  3/48  1/48
                        err_buf[r2][ex - 2] += (qe * 1) / 48;
                        err_buf[r2][ex - 1] += (qe * 3) / 48;
                        err_buf[r2][ex]     += (qe * 5) / 48;
                        err_buf[r2][ex + 1] += (qe * 3) / 48;
                        err_buf[r2][ex + 2] += (qe * 1) / 48;

                        if (out) {
                            frame[page * DISPLAYWIDTH + sx] |= (1 << bit);
                        }
                    }

                    memset(err_buf[r0], 0, (DISPLAYWIDTH + 4) * sizeof(int16_t));
                }

                PACK_DMA_BUF(frame);
                sem_release(&vsync);

                do {
                    DMA_SEND_FRAME();
                    __dmb();
                } while (*(volatile uint8_t *)&display_frame_index == last_fi);

                sem_acquire_blocking(&vsync);
            }
        }

#elif JTBD16_DITHER_MODE == DITHER_STUCKI
        // =========================================================
        // Mode 14: Stucki error-diffusion dithering
        // =========================================================
        // Similar to JJN but with different weight distribution (1/42).
        // Slightly sharper edges than JJN while still smoother than FS.
        //
        //             X   8   4
        //     2   4   8   4   2
        //     1   2   4   2   1
        //
        // All weights sum to 42.
        {
            static uint8_t frame[DISPLAYWIDTH * (DISPLAYHEIGHT / 8)];
            static int16_t err_buf[3][DISPLAYWIDTH + 4]; // +4 for kernel spread

            sem_acquire_blocking(&vsync);

            while (true) {
                uint8_t last_fi = display_frame_index;
                uint8_t *fb = frame_buffer[display_frame_index];

                REBUILD_COMBINED_LUT();
                BUILD_SMOOTH_LUM(fb);
                memset(frame, 0, sizeof(frame));
                memset(err_buf, 0, sizeof(err_buf));

                for (int sy = 0; sy < DISPLAYHEIGHT; sy++) {
                    int page = sy >> 3;
                    int bit  = sy & 7;
                    int r0   = sy % 3;
                    int r1   = (sy + 1) % 3;
                    int r2   = (sy + 2) % 3;

                    for (int sx = 0; sx < DISPLAYWIDTH; sx++) {
                        int ex = sx + 2; // offset into err_buf
                        int lum = LUM_AT(fb, sy, sx);
                        int val = lum + err_buf[r0][ex];
                        int out = (val > JTBD16_DITHER_THRESHOLD) ? 255 : 0;
                        int qe  = val - out;

                        // Stucki kernel: weights/42
                        // Row 0 (current):         X  8/42  4/42
                        err_buf[r0][ex + 1] += (qe * 8) / 42;
                        err_buf[r0][ex + 2] += (qe * 4) / 42;
                        // Row 1: 2/42  4/42  8/42  4/42  2/42
                        err_buf[r1][ex - 2] += (qe * 2) / 42;
                        err_buf[r1][ex - 1] += (qe * 4) / 42;
                        err_buf[r1][ex]     += (qe * 8) / 42;
                        err_buf[r1][ex + 1] += (qe * 4) / 42;
                        err_buf[r1][ex + 2] += (qe * 2) / 42;
                        // Row 2: 1/42  2/42  4/42  2/42  1/42
                        err_buf[r2][ex - 2] += (qe * 1) / 42;
                        err_buf[r2][ex - 1] += (qe * 2) / 42;
                        err_buf[r2][ex]     += (qe * 4) / 42;
                        err_buf[r2][ex + 1] += (qe * 2) / 42;
                        err_buf[r2][ex + 2] += (qe * 1) / 42;

                        if (out) {
                            frame[page * DISPLAYWIDTH + sx] |= (1 << bit);
                        }
                    }

                    memset(err_buf[r0], 0, (DISPLAYWIDTH + 4) * sizeof(int16_t));
                }

                PACK_DMA_BUF(frame);
                sem_release(&vsync);

                do {
                    DMA_SEND_FRAME();
                    __dmb();
                } while (*(volatile uint8_t *)&display_frame_index == last_fi);

                sem_acquire_blocking(&vsync);
            }
        }

#else
#error "JTBD16_DITHER_MODE must be 0-14"
#endif /* JTBD16_DITHER_MODE */

        #undef LUM_AT

#else  // !JTBD16 — original SSD1306 72×40: 3-pass greyscale with parking
        gpio_put(J_OLED_CS, 0);
        gpio_put(J_OLED_DC, 0);
        oled_spi_write_blocking(command_park, sizeof(command_park));

        if (l == 0) {
            sem_acquire_blocking(&vsync);
        }

        {
            uint8_t level = 0x04 >> l;

            for (int p = 0; p < (DISPLAYHEIGHT / 8); ++p) {
                for (int x = 0; x < DISPLAYWIDTH; ++x) {
                    dither ^= 1;
                    uint8_t byte = 0;
                    for (int b = 0; b < 8; ++b) {
                        dither ^= 1;
                        int y = (DISPLAYHEIGHT - 1) - (p * 8 + b);
                        uint8_t *pframe = &frame_buffer[display_frame_index][y * SCREENWIDTH + x];
                        uint lum = display_palette[*pframe];
                        lum = (lum >> 5) + ((lum >> 4) & dither);
                        lum = MIN(lum, 7);
                        byte >>= 1;
                        if (lum & level) {
                            byte |= 0x80;
                        }
                    }
                    field_buffer[p * DISPLAYWIDTH + x] = byte;
                }
            }

            command_run[1] = contrast[l];
        }

        if (++l >= 3) {
            l = 0;
            dither ^= 1;
        }

        if (l == 0) {
            sem_release(&vsync);
        }

        gpio_put(J_OLED_DC, 1);
        oled_spi_write_blocking(field_buffer, sizeof(field_buffer));
        gpio_put(J_OLED_DC, 0);
        oled_spi_write_blocking(command_run, sizeof(command_run));
        gpio_put(J_OLED_CS, 1);

        frame_time = delayed_by_us(frame_time, FRAME_PERIOD);
        sleep_until(frame_time);
#endif  // JTBD16

#else  // !J_OLED_MONO — colour or desktop
#endif // J_OLED_MONO
    }
}

void I_InitGraphics(void)
{
    stbar = resolve_vpatch_handle(VPATCH_STBAR);
    sem_init(&vsync, 1, 1);
    pd_init();


    display_driver_init();

#ifndef J_OLED_COLOUR
    core1_active = true;
    multicore_launch_core1(core1);
#endif

#if USE_ZONE_FOR_MALLOC
    disallow_core1_malloc = true;
#endif
    initialized = true;
}

// Bind all variables controlling video options into the configuration
// file system.
void I_BindVideoVariables(void)
{
//    M_BindIntVariable("use_mouse",                 &usemouse);
//    M_BindIntVariable("fullscreen",                &fullscreen);
//    M_BindIntVariable("video_display",             &video_display);
//    M_BindIntVariable("aspect_ratio_correct",      &aspect_ratio_correct);
//    M_BindIntVariable("integer_scaling",           &integer_scaling);
//    M_BindIntVariable("vga_porch_flash",           &vga_porch_flash);
//    M_BindIntVariable("startup_delay",             &startup_delay);
//    M_BindIntVariable("fullscreen_width",          &fullscreen_width);
//    M_BindIntVariable("fullscreen_height",         &fullscreen_height);
//    M_BindIntVariable("force_software_renderer",   &force_software_renderer);
//    M_BindIntVariable("max_scaling_buffer_pixels", &max_scaling_buffer_pixels);
//    M_BindIntVariable("window_width",              &window_width);
//    M_BindIntVariable("window_height",             &window_height);
//    M_BindIntVariable("grabmouse",                 &grabmouse);
//    M_BindStringVariable("video_driver",           &video_driver);
//    M_BindStringVariable("window_position",        &window_position);
//    M_BindIntVariable("usegamma",                  &usegamma);
//    M_BindIntVariable("png_screenshots",           &png_screenshots);
}

//
// I_StartTic
//
void I_StartTic (void)
{
    if (!initialized)
    {
        return;
    }

    I_GetEvent();
//
//    if (usemouse && !nomouse && window_focused)
//    {
//        I_ReadMouse();
//    }
//
//    if (joywait < I_GetTime())
//    {
//        I_UpdateJoystick();
//    }
}


//
// I_UpdateNoBlit
//
void I_UpdateNoBlit (void)
{
    // what is this?
}

int I_GetPaletteIndex(int r, int g, int b)
{
    return 0;
}

#if !NO_USE_ENDDOOM
void I_Endoom(byte *endoom_data) {
    uint32_t size;
    uint8_t *wa = pd_get_work_area(&size);
    assert(size >=TEXT_SCANLINE_BUFFER_TOTAL_WORDS * 4 + 80*25*2 + 4096);
    text_screen_cpy = wa;
    text_font_cpy = text_screen_cpy + 80 * 25 * 2;
    text_scanline_buffer_start = (uint32_t *) (text_font_cpy + 4096);
#if 0
    static_assert(sizeof(normal_font_data) == 4096, "");
    memcpy(text_font_cpy, normal_font_data, sizeof(normal_font_data));
    memcpy(text_screen_cpy, endoom_data, 80 * 25 * 2);
#else
    static_assert(TEXT_SCANLINE_BUFFER_TOTAL_WORDS * 4 > 1024 + 512, "");
    uint8_t *tmp_buf = (uint8_t *)text_scanline_buffer_start;
    uint16_t *decoder = (uint16_t *)(tmp_buf + 512);
    th_bit_input bi;
    th_bit_input_init(&bi, normal_font_data_z);
    decode_data(text_font_cpy, 4096, &bi, decoder, 512, tmp_buf, 512);
    th_bit_input_init(&bi, endoom_data);
    // text
    decode_data(text_screen_cpy, 80*25, &bi, decoder, 512, tmp_buf, 512);
    // attr
    decode_data(text_screen_cpy+80*25, 80*25, &bi, decoder, 512, tmp_buf, 512);
    static_assert(TEXT_SCANLINE_BUFFER_TOTAL_WORDS * 4 > 80*25*2, "");
    // re-interlace the text & attr
    memcpy(tmp_buf, text_screen_cpy, 80*25*2);
    for(int i=0;i<80*25;i++) {
        text_screen_cpy[i*2] = tmp_buf[i];
        text_screen_cpy[i*2+1] = tmp_buf[80*25 + i];
    }
#endif
    text_screen_data = text_screen_cpy;
}
#endif

void I_GraphicsCheckCommandLine(void)
{

}

// Check if we have been invoked as a screensaver by xscreensaver.

void I_CheckIsScreensaver(void)
{
}

void I_DisplayFPSDots(boolean dots_on)
{
}


#endif

