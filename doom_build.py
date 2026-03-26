"""
PlatformIO build script for rp2040-doom on TBD-16.
Adds doom engine source files and include paths to the PlatformIO build.
"""
Import("env")
import os

project_dir = env["PROJECT_DIR"]
doom_base = os.path.join(project_dir, "lib", "rp2040-doom")
doom_src = os.path.join(doom_base, "src")

# PlatformIO always compiles pico_stdio_usb even without USB stdio.
# Provide TinyUSB headers so it compiles (dead code, removed by --gc-sections).
pio_sdk = env.PioPlatform().get_package_dir("framework-picosdk")
tusb_src = os.path.join(pio_sdk, "lib", "tinyusb", "src")
if os.path.isdir(tusb_src):
    env.Append(CPPPATH=[tusb_src])
    env.Append(CPPDEFINES=[
        ("CFG_TUSB_MCU", "OPT_MCU_RP2040"),
        ("CFG_TUSB_OS", "OPT_OS_PICO"),
        ("CFG_TUD_ENABLED", 1),
        ("CFG_TUD_CDC", 1),
        ("CFG_TUD_CDC_RX_BUFSIZE", 256),
        ("CFG_TUD_CDC_TX_BUFSIZE", 256),
    ])
doom_opl = os.path.join(doom_base, "opl")

# Include paths — config.h comes from project src/ dir, doom sources from submodule
env.Append(CPPPATH=[
    os.path.join(project_dir, "src"),          # config.h
    doom_src,                                    # doom core headers
    os.path.join(doom_src, "doom"),             # game engine headers
    os.path.join(doom_src, "pico"),             # pico platform headers
    doom_opl,                                    # OPL headers
    os.path.join(doom_src, "adpcm-xq"),         # ADPCM headers
    os.path.join(doom_base, "textscreen"),       # textscreen headers (for types)
    os.path.join(doom_base, "boards"),           # board headers (jtbd16.h)
    os.path.join(project_dir, "lib", "no-OS-FatFS-SD-SDIO-SPI-RPi-Pico", "src", "include"),  # FatFs library headers
    os.path.join(project_dir, "lib", "no-OS-FatFS-SD-SDIO-SPI-RPi-Pico", "src", "sd_driver"),  # sd_card.h
    os.path.join(project_dir, "lib", "no-OS-FatFS-SD-SDIO-SPI-RPi-Pico", "src", "ff15", "source"),  # ff.h
])

# Doom game engine sources (src/doom/)
doom_game_files = [
    "am_map.c", "deh_ammo.c", "deh_bexstr.c", "deh_cheat.c", "deh_doom.c",
    "deh_frame.c", "deh_misc.c", "deh_ptr.c", "deh_sound.c", "deh_thing.c",
    "deh_weapon.c", "d_items.c", "d_main.c", "d_net.c", "doomdef.c",
    "doomstat.c", "dstrings.c", "f_finale.c", "f_wipe.c", "g_game.c",
    "hu_lib.c", "hu_stuff.c", "info.c", "m_menu.c", "m_random.c",
    "p_ceilng.c", "p_doors.c", "p_enemy.c", "p_floor.c", "p_inter.c",
    "p_lights.c", "p_map.c", "p_maputl.c", "p_mobj.c", "p_plats.c",
    "p_pspr.c", "p_saveg.c", "p_setup.c", "p_sight.c", "p_spec.c",
    "p_switch.c", "p_telept.c", "p_tick.c", "p_user.c", "r_bsp.c",
    "r_data.c", "r_data_whd.c", "r_draw.c", "r_main.c", "r_plane.c",
    "r_segs.c", "r_sky.c", "r_things.c", "s_sound.c", "sounds.c",
    "statdump.c", "st_lib.c", "st_stuff.c", "wi_stuff.c",
]

# Core support sources (src/)
doom_core_files = [
    "aes_prng.c", "d_event.c", "d_iwad.c", "d_loop.c", "d_mode.c",
    "deh_str.c", "gusconf.c", "i_main.c", "i_oplmusic.c", "i_sound.c",
    "m_argv.c", "m_bbox.c", "m_cheat.c", "m_config.c", "m_controls.c",
    "m_fixed.c", "m_misc.c", "memio.c", "midifile.c", "mus2mid.c",
    "net_client.c", "sha1.c", "tables.c", "tiny_huff.c", "musx_decoder.c",
    "image_decoder.c", "v_diskicon.c", "v_video.c", "w_checksum.c",
    "w_file.c", "w_file_memory.c", "w_file_posix.c", "w_main.c",
    "w_merge.c", "w_wad.c", "z_zone.c",
]

# Pico platform sources (src/pico/)
doom_pico_files = [
    "i_glob.c", "i_input.c", "i_system.c", "i_timer.c", "i_video.c",
    "piconet.c", "stubs.c",
    "blit.S",
]

# OPL emulator sources (opl/)
opl_files = [
    "opl_api.c", "emu8950.c", "opl_pico.c", "slot_render.cpp",
    "slot_render_pico.S",
]

# ADPCM (src/adpcm-xq/)
adpcm_files = ["adpcm-lib.c"]

# Render
render_files = ["pd_render.cpp"]

# Build all doom sources into the build directory
def add_sources(subdir, files, build_name):
    src_dir = os.path.join(doom_src, subdir) if subdir else doom_src
    full_paths = [os.path.join(src_dir, f) for f in files]
    # Filter to only existing files
    existing = [f for f in full_paths if os.path.exists(f)]
    if existing:
        # Create a src_filter from the file list
        src_filter = ["-<*>"] + ["+<%s>" % f for f in files]
        env.BuildSources(
            os.path.join("$BUILD_DIR", build_name),
            src_dir,
            src_filter=" ".join(src_filter)
        )

add_sources("doom", doom_game_files, "doom_game")
add_sources("", doom_core_files, "doom_core")
add_sources("pico", doom_pico_files, "doom_pico")
add_sources("", render_files, "doom_render")
add_sources("adpcm-xq", adpcm_files, "doom_adpcm")

# OPL is in a different directory tree
opl_existing = [f for f in opl_files if os.path.exists(os.path.join(doom_opl, f))]
if opl_existing:
    opl_filter = ["-<*>"] + ["+<%s>" % f for f in opl_files]
    env.BuildSources(
        os.path.join("$BUILD_DIR", "doom_opl"),
        doom_opl,
        src_filter=" ".join(opl_filter)
    )

# Petit FatFS (for SD card WAD loading)
petit_fatfs_dir = os.path.join(project_dir, "lib", "petit_fatfs")
petit_fatfs_files = ["pff.c", "diskio.c"]
pf_existing = [f for f in petit_fatfs_files if os.path.exists(os.path.join(petit_fatfs_dir, f))]
if pf_existing:
    pf_filter = ["-<*>"] + ["+<%s>" % f for f in petit_fatfs_files]
    env.BuildSources(
        os.path.join("$BUILD_DIR", "petit_fatfs"),
        petit_fatfs_dir,
        src_filter=" ".join(pf_filter)
    )

# Pre-link: weaken SDK's __wrap_printf etc. so our no-ops in doom_stub.c win.
# The SDK's pico_stdio defines strong __wrap_printf that routes through its
# stdio infrastructure (which calls malloc/Z_Malloc and crashes). By weakening
# those symbols, the linker prefers our strong no-op definitions.
def weaken_sdk_stdio(source, target, env):
    import subprocess
    cc = env.subst("$CC")
    objcopy = cc.replace("arm-none-eabi-gcc", "arm-none-eabi-objcopy")
    stdio_o = os.path.join(env.subst("$BUILD_DIR"), "PicoSDKpico_stdio", "stdio.o")
    if os.path.exists(stdio_o):
        cmd = [objcopy]
        for sym in ["__wrap_printf", "__wrap_vprintf", "__wrap_puts", "__wrap_putchar"]:
            cmd += ["--weaken-symbol", sym]
        cmd.append(stdio_o)
        subprocess.check_call(cmd)

env.AddPreAction("$BUILD_DIR/${PROGNAME}.elf", weaken_sdk_stdio)
