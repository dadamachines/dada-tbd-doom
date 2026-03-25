# Doom on DaDa Machines TBD-16

A port of [rp2040-doom](https://github.com/kilograham/rp2040-doom) to the [DaDa Machines TBD-16](https://dadamachines.com) synthesizer hardware, driving its 2.4" SSD1309 128x64 monochrome OLED with phase-shifted blue noise temporal dithering for smooth greyscale rendering.

## Hardware

| Component | Details |
|---|---|
| **MCU** | RP2350 (dual Cortex-M33, 150 MHz) |
| **Display** | SSD1309 128x64 OLED, 2.4", monochrome, SPI1 at 8 MHz |
| **Input** | STM32 UI board via I2C1 (D-pad, buttons, encoders) |
| **Audio** | Not yet implemented (stubs present) |
| **Storage** | Doom WAD embedded in flash (doom1.whx) |

## What's Different from Upstream rp2040-doom

The upstream [rp2040-doom](https://github.com/kilograham/rp2040-doom) targets the Thumby (RP2040, SSD1306 72x40). This port adapts it to the TBD-16:

- **RP2350** instead of RP2040 (Cortex-M33 vs M0+, significantly more SRAM)
- **SSD1309 128x64** instead of SSD1306 72x40 (4x the pixels, different controller init)
- **I2C button input** from STM32 UI board instead of direct GPIO
- **PlatformIO build system** instead of CMake -- see platformio.ini and doom_build.py
- **4-frame phase-shifted blue noise dithering** instead of the original 3-pass contrast-switching greyscale

### Display Rendering

The original rp2040-doom switches between SSD1306 contrast levels to produce greyscale on a tiny 72x40. The SSD1309 on TBD-16 is a 2.4" panel with large visible pixels and no VSYNC signal -- the original approach caused heavy flickering and visible scanlines.

The current renderer uses **4 temporal frames with spatially-offset blue noise thresholds**, cycled rapidly on core 1:

1. A **16x16 void-and-cluster blue noise** texture provides natural film-grain dithering (no grid artifacts like Bayer)
2. Each of the **4 frames samples at a different spatial offset** -- offsets (0,0), (7,3), (3,11), (11,7) are coprime to the 16-pixel tile size, breaking moire patterns against Doom's repeating wall textures
3. The eye integrates all 4 frames -- **5 effective grey levels** per pixel
4. All frames have similar average brightness -- **minimal flicker** from the SSD1309's asynchronous scanning
5. A **shadow-lift LUT** (gamma 0.625) opens up dark corridor areas common in Doom

### Display Orientation

The SSD1309 on TBD-16 is physically mounted 180 degrees rotated. The driver uses:
- Segment remap 0xA1 (horizontal flip)
- Normal COM scan 0xC0 + software Y-flip in the rendering loop

## Building

### Prerequisites

- [PlatformIO](https://platformio.org/) (CLI or VS Code extension)
- A **CMSIS-DAP debug probe** connected to the TBD-16 (e.g. Raspberry Pi Debug Probe)

### Build & Flash

```bash
# Build and upload via debug probe (recommended)
pio run -t upload

# Build only (output in .pio/build/doom-tbd16/)
pio run
```

> **Important:** Always flash via the CMSIS-DAP debug probe (pio run -t upload). The WAD data handling and flash layout require the debug probe workflow. Do not use UF2 drag-and-drop.

### How the Build Works

The build is orchestrated by PlatformIO with two custom scripts:

1. **doom_build.py** (pre-build) -- Adds the rp2040-doom engine source files and include paths to the PlatformIO build. This replaces the upstream CMake build system.
2. **append_wad_uf2.py** (post-build) -- Appends the compressed WAD data (doom1.whx) to the firmware image.

The PlatformIO environment doom-tbd16 uses:
- **Platform:** maxgerhardt/platform-raspberrypi (RP2350 support)
- **Board:** generic_rp2350
- **Framework:** Pico SDK with earlephilhower core
- **Upload protocol:** cmsis-dap

## Project Structure

```
platformio.ini          # PlatformIO project config (env: doom-tbd16)
doom_build.py           # Pre-build: adds doom engine sources to PIO
append_wad_uf2.py       # Post-build: appends WAD data to UF2

src/
    config.h            # Doom compile-time configuration
    doom_stub.c         # Minimal stubs for PIO compilation
    i_picosound_stub.c  # Sound system stubs

lib/
    rp2040-doom/        # Upstream doom engine (modified fork)
        boards/
            jtbd16.h    # TBD-16 board definition (pins, display, I2C, buttons)
        src/pico/
            i_video.c   # Display driver -- blue noise dithering engine (core 1)
            i_input.c   # I2C button input from STM32 UI board
            i_system.c  # System-level RP2350 hooks
    i2ckbd/             # I2C keyboard driver for STM32 UI board
    lcdspi/             # SSD1309 OLED driver (UI mode, not used during Doom)

data/
    doom1.whx           # Compressed Doom 1 shareware WAD

gen_blue_noise2.py      # Blue noise texture generator (void-and-cluster, pure Python)
gen_remap_lut.py        # Shadow-lift LUT generator
button-mapping.md       # Physical button layout and Doom control mapping
pinmap.md               # Full hardware pin map (RP2350, ESP32-P4, STM32)
```

### Key Source Files

| File | Purpose |
|---|---|
| lib/rp2040-doom/boards/jtbd16.h | All pin definitions, display config, I2C addresses, button bit indices |
| lib/rp2040-doom/src/pico/i_video.c | Core 1 rendering pipeline -- SSD1309 init, blue noise dithering, frame cycling |
| lib/rp2040-doom/src/pico/i_input.c | Reads STM32 UI board buttons over I2C, maps to Doom controls |
| doom_build.py | PlatformIO build script -- replaces the upstream CMake system |

## Controls

| Button | Doom Action |
|---|---|
| D-pad Up/Down | Move forward / backward |
| D-pad Left/Right | Turn left / right |
| FUNC5 (A) | Fire |
| FUNC6 (B) | Use / Open doors |
| MASTER (X) | Run |
| SOUND (Y) | Strafe |
| D-pad Down + Y | Switch weapon |

See [button-mapping.md](button-mapping.md) for the full physical panel layout.

## Credits

- **[rp2040-doom](https://github.com/kilograham/rp2040-doom)** by Graham Sanderson -- the RP2040 Doom port this is based on
- **[Chocolate Doom](https://www.chocolate-doom.org/)** -- the upstream source port
- **[DaDa Machines](https://dadamachines.com)** -- TBD-16 hardware

## License

See [COPYING.md](lib/rp2040-doom/COPYING.md) -- GPL v2, inherited from Chocolate Doom / rp2040-doom.
