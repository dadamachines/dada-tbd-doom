# Doom on DaDa Machines TBD-16

A port of [rp2040-doom](https://github.com/kilograham/rp2040-doom) to the [DaDa Machines TBD-16](https://dadamachines.com) synthesizer, running on the RP2350 co-processor and rendering to a 2.4" SSD1309 128×64 monochrome OLED with 15 selectable dithering algorithms for 1-bit greyscale.

## Hardware

| Component | Details |
|---|---|
| **MCU** | RP2350B (dual Cortex-M33, 150 MHz, 48-GPIO variant) |
| **Display** | SSD1309 128×64 OLED, 2.4", monochrome, PIO SPI at 10 MHz |
| **Input** | STM32 UI board via I2C1 (D-pad, buttons, encoders) |
| **Audio** | OPL + SFX mixed at 49716 Hz → SPI DMA to ESP32-P4 codec (44100 Hz I2S) |
| **PSRAM** | 8 MB APS6404 on QMI CS1 (GPIO19), memory-mapped at 0x11000000 |
| **Storage** | SD card (Petit FatFS) — WAD loaded into PSRAM at boot |

The TBD-16 is a triple-processor platform: **ESP32-P4** (main DSP, audio codec), **RP2350** (UI, OLED, MIDI), and **STM32F0** (button/encoder scanning). Doom runs entirely on the RP2350 — Core 0 for game logic, Core 1 for display rendering.

---

## Installation

### 1. Prepare the SD Card

1. Insert the TBD-16 SD card into your computer
2. Create a `DATA` folder in the root (uppercase)
3. Copy `DOOM1.WHX` into that folder
4. Eject the SD card and insert it back into the TBD-16

The file must be at: `SD:/DATA/DOOM1.WHX`

> **Note:** The WAD file is not included in this repository. You need a copy of the Doom 1 shareware WAD converted to WHX format.

### 2. Flash the Firmware

**Option A — BOOTSEL drag-and-drop:**
1. Hold BOOTSEL while connecting USB → a **RPI-RP2** drive appears
2. Drag `firmware.uf2` from `.pio/build/doom-tbd16/` onto the drive
3. Device reboots, Doom starts automatically

**Option B — Debug probe:**
```bash
pio run -t upload
```

At boot the firmware loads the WAD from SD into 8 MB PSRAM (~2 seconds), then starts the game.

---

## Controls

| Button | Action |
|---|---|
| D-pad Up/Down | Move forward / backward |
| D-pad Left/Right | Turn left / right |
| A (F5) | Fire |
| B (F6) | Use / Open doors |
| X (Master) | Strafe modifier (hold + Left/Right) |
| Y (Sound) | Run modifier (hold + direction) |
| PLAY | Pause / Menu |
| REC | Toggle automap |
| S1 / S2 | Previous / Next weapon |

See [docs/MANUAL.md](docs/MANUAL.md) for the full user manual and [docs/BUTTON_MAPPING.md](docs/BUTTON_MAPPING.md) for the I2C protocol details.

---

## Dithering Modes

The display pipeline converts 8-bit luminance to 1-bit pixels using one of 15 dithering algorithms. All parameters are compile-time constants in [`lib/rp2040-doom/boards/jtbd16.h`](lib/rp2040-doom/boards/jtbd16.h):

```c
#define JTBD16_DITHER_MODE       0      // 0-14 (see table below)
#define JTBD16_SHADOW_GAMMA      0      // 0=pow(0.50) 1=pow(0.625) 2=pow(0.80) 3=linear
#define JTBD16_DITHER_THRESHOLD  110    // 0-255 black/white point (lower=brighter)
#define JTBD16_EDGE_STRENGTH     48     // 0-128 sharpening (mode 4 only)
#define JTBD16_HUD_Y_START       52     // HUD region start row (mode 5 only)
#define JTBD16_HUD_THRESHOLD     100    // HUD threshold (mode 5 only)
#define JTBD16_BN_MODULATION     0      // 0-128 blue noise perturbation (modes 8,9)
#define JTBD16_PREDITHER_SMOOTH  0      // 0=off 1=on — 3×3 Gaussian anti-moiré filter
```

| Mode | Name | Description |
|---:|---|---|
| 0 | Atkinson | Error diffusion — clean surfaces, sharp edges. Default. |
| 1 | Blue Noise Static | Threshold with 64×64 blue noise texture |
| 2 | Blue Noise Temporal | Animated blue noise (frame-cycling) |
| 3 | 3-Pass Contrast | Multi-pass with contrast enhancement |
| 4 | Blue Noise + Edge | Blue noise threshold with Sobel edge sharpening |
| 5 | Hybrid HUD | Different threshold for HUD region vs gameplay |
| 6 | Floyd-Steinberg | Classic error diffusion |
| 7 | Sierra Lite | Lightweight error diffusion |
| 8 | BN + Floyd-Steinberg | Blue noise modulated Floyd-Steinberg |
| 9 | BN + Atkinson | Blue noise modulated Atkinson |
| 10 | Bayer 4×4 | Ordered dither, 4×4 matrix |
| 11 | Bayer 8×8 | Ordered dither, 8×8 matrix |
| 12 | Serpentine FS | Floyd-Steinberg with alternating scan direction |
| 13 | JJN | Jarvis-Judice-Ninke error diffusion |
| 14 | Stucki | Stucki error diffusion |

Change the mode, rebuild (`pio run`), and flash. See [docs/DITHERING.md](docs/DITHERING.md) for observations and tuning notes.

---

## Architecture

```
┌─────────────────────────────────────────────────┐
│                    RP2350B                       │
│                                                  │
│  Core 0 (Game)              Core 1 (Display)     │
│  ┌──────────────┐          ┌──────────────────┐  │
│  │ Doom engine   │ shared  │ Dithering        │  │
│  │ OPL + SFX mix │───buf──▶│ PIO SPI @ 10 MHz │  │
│  │ SD WAD loader │         │ DMA to SSD1309   │  │
│  └──────┬───────┘          └──────────────────┘  │
│         │ SPI1 DMA                                │
│         ▼                                         │
│  ┌──────────────┐                                │
│  │ ESP32-P4      │  Resamples 49716→44100 Hz     │
│  │ TLV320AIC3254 │  I2S stereo 32-bit output     │
│  └──────────────┘                                │
│                                                  │
│  PSRAM 8 MB (APS6404)  ◀── WAD loaded from SD   │
│  QMI CS1, GPIO19            at boot via SPI0     │
│  Mapped at 0x11000000       (Petit FatFS)        │
└─────────────────────────────────────────────────┘
```

### Key Subsystems

- **Display:** Core 1 runs a 60 Hz render loop. The game engine (Core 0) writes 8-bit luminance into a shared buffer. Core 1 applies the selected dithering algorithm, packs 1-bit pixels, and DMA-transfers the framebuffer to the SSD1309 via PIO SPI at 10 MHz.

- **Audio:** Core 0 mixes OPL music and SFX at 49716 Hz. A TIMER1 ISR at 5000 Hz packs 32 stereo sample pairs into SPI1 DMA frames, paced by the P4 codec word clock (GPIO27). The ESP32-P4's PicoAudioBridge plugin resamples to 44100 Hz for I2S output.

- **PSRAM:** 8 MB APS6404 on QMI CS1, memory-mapped. The WAD file is loaded from SD card into PSRAM at boot, then PSRAM is accessed as flat memory by the Doom engine. Initialization requires clearing the RP2350 pad isolation (ISO) bit — see [docs/TECHNICAL.md](docs/TECHNICAL.md).

- **Input:** STM32F0 scans buttons and encoders, presents them as a 12-bit register over I2C1 at address 0x42. The RP2350 polls at frame rate and maps to Doom controls.

---

## Building

### Prerequisites

- [PlatformIO](https://platformio.org/) (CLI or VS Code extension)
- A **CMSIS-DAP debug probe** (for upload via `pio run -t upload`)

### Build & Flash

```bash
# Build only
pio run

# Build and upload via debug probe
pio run -t upload
```

The build is orchestrated by PlatformIO with a single custom script:

- **`doom_build.py`** (pre-build) — Adds rp2040-doom engine source files and include paths to the PlatformIO build, replacing the upstream CMake system.

The WAD (`DOOM1.WHX`) must be on the SD card at `/DATA/DOOM1.WHX`. It is loaded into PSRAM at boot.

---

## Project Structure

```
platformio.ini          # PlatformIO config (env: doom-tbd16)
doom_build.py           # Pre-build: adds doom engine sources to PIO

src/
    config.h            # Doom compile-time configuration
    doom_stub.c         # Minimal stubs for PIO compilation
    i_picosound.c       # Sound system implementation
    pico_audio_bridge.c # Audio SPI transport to ESP32-P4
    psram_init.c        # PSRAM APS6404 init (zero SDK headers, raw registers)
    sd_wad_loader.c     # SD card WAD loading into PSRAM
    pio_spi_oled.c      # PIO SPI OLED driver with DMA
    p4_spi_transport.c  # SPI transport to ESP32-P4
    p4_control_link.c   # P4 control protocol

lib/
    rp2040-doom/        # Upstream doom engine (modified fork)
        boards/
            jtbd16.h    # TBD-16 board definition (pins, dithering config)
        src/pico/
            i_video.c   # Core 1 display pipeline — dithering engine
            i_input.c   # I2C button input from STM32 UI board
    i2ckbd/             # I2C keyboard driver for STM32 UI board
    lcdspi/             # SSD1309 driver (UI mode, not used during Doom)
    petit_fatfs/        # Petit FatFS for SD card WAD loading

data/
    doom1.whx           # Compressed Doom 1 shareware WAD (not in repo)

docs/                   # See Documentation section below
tools/                  # Build, flash, and analysis utilities
```

### Key Source Files

| File | Purpose |
|---|---|
| `lib/rp2040-doom/boards/jtbd16.h` | Pin definitions, dithering config, I2C addresses, button indices |
| `lib/rp2040-doom/src/pico/i_video.c` | Core 1 render loop — all 15 dithering modes, DMA frame transfer |
| `src/psram_init.c` | PSRAM init with raw register access (no SDK headers) |
| `src/pio_spi_oled.c` | PIO SPI driver for SSD1309 at 10 MHz |
| `src/pico_audio_bridge.c` | Audio SPI DMA transport to P4 |
| `src/sd_wad_loader.c` | Boot-time WAD loading from SD into PSRAM |
| `doom_build.py` | PlatformIO build script replacing upstream CMake |

---

## Documentation

| Document | Description |
|---|---|
| [TECHNICAL.md](docs/TECHNICAL.md) | Hardware architecture, pin map, PSRAM gotchas, register details |
| [DITHERING.md](docs/DITHERING.md) | Dithering algorithm reference and test observations |
| [VIDEO_PIPELINE.md](docs/VIDEO_PIPELINE.md) | Complete video pipeline from engine to OLED |
| [TEMPORAL_DITHERING_REVIEW.md](docs/TEMPORAL_DITHERING_REVIEW.md) | Design review for greyscale simulation quality |
| [AUDIO_BRIDGE.md](docs/AUDIO_BRIDGE.md) | Audio transport architecture (RP2350 → P4 codec) |
| [AUDIO_AUDIT.md](docs/AUDIO_AUDIT.md) | Audio quality audit vs original rp2040-doom |
| [MANUAL.md](docs/MANUAL.md) | User manual — controls, gameplay tips |
| [BUTTON_MAPPING.md](docs/BUTTON_MAPPING.md) | I2C protocol and physical button layout |
| [PINMAP.md](docs/PINMAP.md) | Full hardware pin map (RP2350, ESP32-P4, STM32) |

---

## What's Different from Upstream rp2040-doom

The upstream [rp2040-doom](https://github.com/kilograham/rp2040-doom) targets the Thumby (RP2040, SSD1306 72×40). This port:

- **RP2350B** instead of RP2040 (Cortex-M33 vs M0+, more SRAM, 48 GPIOs)
- **SSD1309 128×64** instead of SSD1306 72×40 (4× the pixels, different controller)
- **PIO SPI at 10 MHz** instead of hardware SPI — consistent timing, DMA-driven
- **SD card WAD loading** into 8 MB PSRAM instead of flash-embedded WAD
- **I2C button input** from STM32 UI board instead of direct GPIO
- **Audio over SPI** to ESP32-P4 codec instead of PWM/I2S on-chip
- **PlatformIO build** instead of CMake
- **15 dithering algorithms** with compile-time selection

The original uses SSD1306 contrast cycling to produce greyscale on a tiny 72×40 display. The SSD1309 on TBD-16 is a 2.4" panel with large visible pixels and no VSYNC — that approach caused heavy flickering. Instead, this port uses spatial dithering algorithms (error diffusion, blue noise, ordered dither) to convert 8-bit luminance to clean 1-bit output.

---

## Credits

- **[rp2040-doom](https://github.com/kilograham/rp2040-doom)** by Graham Sanderson — the RP2040 Doom port this is based on
- **[Chocolate Doom](https://www.chocolate-doom.org/)** — the upstream source port
- **[DaDa Machines](https://dadamachines.com)** — TBD-16 hardware

## License

See [COPYING.md](lib/rp2040-doom/COPYING.md) — GPL v2, inherited from Chocolate Doom / rp2040-doom.
