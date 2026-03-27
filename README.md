# Doom on DaDa Machines TBD-16

A port of [rp2040-doom](https://github.com/kilograham/rp2040-doom) to the [DaDa Machines TBD-16](https://dadamachines.com) synthesizer hardware, driving its 2.4" SSD1309 128x64 monochrome OLED with configurable dithering (15 algorithms) for 1-bit greyscale rendering.

## Hardware

| Component | Details |
|---|---|
| **MCU** | RP2350 (dual Cortex-M33, 150 MHz) |
| **Display** | SSD1309 128x64 OLED, 2.4", monochrome, SPI1 at 8 MHz |
| **Input** | STM32 UI board via I2C1 (D-pad, buttons, encoders) |
| **Audio** | Doom audio via SPI to P4 codec (PicoAudioBridge) — see [docs/AUDIO_BRIDGE.md](docs/AUDIO_BRIDGE.md) |
| **PSRAM** | 8 MB APS6404 on QMI CS1 (GPIO19) |
| **Storage** | Doom WAD loaded from SD card into PSRAM at boot |

## Installation (End Users)

You need two things: the firmware UF2 and the Doom WAD file on your SD card.

### 1. Prepare the SD Card

1. Insert the TBD-16 SD card into your computer
2. Create a folder called `data` in the root of the SD card
3. Copy `doom1.whx` into that folder
4. Eject the SD card and insert it back into the TBD-16

The file should be at: `SD:/data/doom1.whx`

### 2. Flash the Firmware

1. Put the TBD-16 into BOOTSEL mode (hold BOOTSEL while connecting USB)
2. A USB drive called **RPI-RP2** will appear on your computer
3. Drag and drop `firmware.uf2` onto the **RPI-RP2** drive
4. The device will reboot and Doom will start automatically

The firmware is only ~500 KB. At boot it loads the WAD from the SD card into the 8 MB PSRAM, which takes about 2 seconds.

> **Note:** The WAD file is not included in this repository. You need a copy of the Doom 1 shareware WAD converted to WHX format.

---

## What's Different from Upstream rp2040-doom

The upstream [rp2040-doom](https://github.com/kilograham/rp2040-doom) targets the Thumby (RP2040, SSD1306 72x40). This port adapts it to the TBD-16:

- **RP2350** instead of RP2040 (Cortex-M33 vs M0+, significantly more SRAM)
- **SSD1309 128x64** instead of SSD1306 72x40 (4x the pixels, different controller init)
- **I2C button input** from STM32 UI board instead of direct GPIO
- **PlatformIO build system** instead of CMake -- see platformio.ini and doom_build.py
- **15 dithering algorithms** with compile-time switching -- see [docs/DITHERING.md](docs/DITHERING.md)

### Display Rendering

The original rp2040-doom switches between SSD1306 contrast levels to produce greyscale on a tiny 72x40. The SSD1309 on TBD-16 is a 2.4" panel with large visible pixels and no VSYNC signal -- the original approach caused heavy flickering and visible scanlines.

The default renderer uses **Atkinson error-diffusion dithering** (mode 0), which produces clean surfaces with sharp edges. 14 other algorithms are available for comparison -- see [docs/DITHERING.md](docs/DITHERING.md) for the full list and build instructions.

A configurable **shadow-lift gamma LUT** opens up dark corridor areas common in Doom.

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
# Build only (output: .pio/build/doom-tbd16/firmware.uf2)
pio run

# Build and upload via debug probe
pio run -t upload

# Flash via debug probe (without rebuilding)
./tools/flash.sh firmware
```

The firmware UF2 (~500 KB) can also be flashed via BOOTSEL drag-and-drop — see **Installation** above.

The Doom WAD (`doom1.whx`) must be placed on the SD card at `/data/doom1.whx`. It is loaded into PSRAM at boot.

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

docs/
    AUDIO_BRIDGE.md     # Audio transport architecture (RP2350 → P4 codec)
    DITHERING.md        # Dithering algorithm reference and test log
    MANUAL.md           # User manual -- controls, gameplay tips
    TECHNICAL.md        # Hardware architecture and engineering notes
    button-mapping.md   # I2C protocol and physical button layout
    pinmap.md           # Full hardware pin map (RP2350, ESP32-P4, STM32)

tools/
    gen_blue_noise2.py  # Blue noise texture generator (void-and-cluster)
    gen_remap_lut.py    # Shadow-lift LUT generator
    flash.sh            # Flash via debug probe
    serial_capture.py   # UART debug capture
    check_uf2.py        # UF2 file inspector
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
| A (F5) | Fire |
| B (F6) | Use / Open doors |
| X (Master) | Strafe modifier (hold + Left/Right to sidestep) |
| Y (Sound) | Run modifier (hold + direction to move faster) |
| PLAY | Pause / Menu |
| REC | Toggle automap |
| S1 | Previous weapon |
| S2 | Next weapon |

See [docs/MANUAL.md](docs/MANUAL.md) for the full user manual and [docs/button-mapping.md](docs/button-mapping.md) for the hardware I2C protocol.

## Credits

- **[rp2040-doom](https://github.com/kilograham/rp2040-doom)** by Graham Sanderson -- the RP2040 Doom port this is based on
- **[Chocolate Doom](https://www.chocolate-doom.org/)** -- the upstream source port
- **[DaDa Machines](https://dadamachines.com)** -- TBD-16 hardware

## License

See [COPYING.md](lib/rp2040-doom/COPYING.md) -- GPL v2, inherited from Chocolate Doom / rp2040-doom.
