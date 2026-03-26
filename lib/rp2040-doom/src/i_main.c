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
//	Main program, simply calls D_DoomMain high level loop.
//

#include "config.h"

#include <stdlib.h>
#include <stdio.h>

#if !LIB_PICO_STDLIB
#include "SDL.h"
#else
#include "pico/stdlib.h"
#include "hardware/gpio.h"
#include "pico/sem.h"
#include "pico/multicore.h"
#if PICO_ON_DEVICE
#include "hardware/vreg.h"
#endif
#endif
#if USE_PICO_NET
#include "piconet.h"
#endif
#include "doomtype.h"
#include "i_system.h"
#include "m_argv.h"
#if USE_SD_WAD
extern int sd_wad_load(void);
#endif

//
// D_DoomMain()
// Not a globally visible function, just included for source reference,
// calls all startup code, parses command line options.
//

void D_DoomMain (void);

#if PICO_ON_DEVICE
#include "pico/binary_info.h"
// I2S audio not present on TBD-16; GPIO20 is UART1 TX (debug probe)
extern void power_on_logo(void);
#endif

extern void debug_uart_init(void);

int main(int argc, char **argv)
{
    // save arguments
#if !NO_USE_ARGS
    myargc = argc;
    myargv = argv;
#endif
#if PICO_ON_DEVICE
    debug_uart_init();   // UART1 TX on GPIO20 → debug probe (before anything else)
/*    
    vreg_set_voltage(VREG_VOLTAGE_1_30);
    // todo pause? is this the cause of the cold start isue?

    set_sys_clock_khz(270000, true);
*/
    power_on_logo();
    extern void debug_show_stage(int);
    debug_show_stage(1); // Stage 1: after power_on_logo
#if JTBD16_BOOT_DEBUG
    sleep_ms(500);
#endif
#if !USE_PICO_NET
    // debug ?
//    gpio_debug_pins_init();
#endif
#ifdef PICO_SMPS_MODE_PIN
    gpio_init(PICO_SMPS_MODE_PIN);
    gpio_set_dir(PICO_SMPS_MODE_PIN, GPIO_OUT);
    gpio_put(PICO_SMPS_MODE_PIN, 1);
#endif
#endif
#if LIB_PICO_STDIO
    debug_show_stage(2); // Stage 2: before stdio_init_all
    stdio_init_all();
    printf("[DOOM] stdio OK\n");
#endif
#if USE_SD_WAD
    debug_show_stage(5); // Stage 5: loading WAD from SD card
    printf("[DOOM] Loading WAD from SD card...\n");
    if (sd_wad_load() != 0) {
        panic("[DOOM] Failed to load WAD from SD card!");
    }
    printf("[DOOM] SD WAD load complete\n");
#endif
#if PICO_BUILD
    debug_show_stage(3); // Stage 3: before I_Init
    I_Init();
    printf("[DOOM] I_Init OK\n");
#endif
#if USE_PICO_NET
    // do init early to set pulls
    piconet_init();
#endif
//!
    // Print the program version and exit.
    //
    if (M_ParmExists("-version") || M_ParmExists("--version")) {
        puts(PACKAGE_STRING);
        exit(0);
    }

#if !NO_USE_ARGS
    M_FindResponseFile();
#endif

    #ifdef SDL_HINT_NO_SIGNAL_HANDLERS
    SDL_SetHint(SDL_HINT_NO_SIGNAL_HANDLERS, "1");
    #endif

    // start doom
    debug_show_stage(4); // Stage 4: before D_DoomMain
    printf("[DOOM] calling D_DoomMain...\n");
    D_DoomMain ();

    return 0;
}

