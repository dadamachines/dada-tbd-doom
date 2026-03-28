# TBD-16 Doom Port — Technical Reference

Accumulated engineering knowledge from porting rp2040-doom to the DaDa Machines TBD-16.

---

## 1. Hardware Architecture

The TBD-16 is a triple-processor synthesizer platform:

| Processor | Role | Connection |
|---|---|---|
| **ESP32-P4** | Main DSP engine, audio codec, WiFi | SPI0/SPI1 to RP2350 |
| **RP2350** | UI, OLED display, MIDI, app runtime | SPI to P4, I2C to STM32, SPI to OLED |
| **STM32F0** | Button/encoder scanning | I2C1 slave at 0x42 |

Doom runs entirely on the RP2350. Core 0 runs the game logic, core 1 handles display rendering.

### RP2350 Pin Assignments (Doom-relevant)

| Function | GPIO | Peripheral |
|---|---|---|
| OLED SCLK | 14 | SPI1 |
| OLED MOSI | 15 | SPI1 |
| OLED CS | 13 | GPIO (active low) |
| OLED DC | 12 | GPIO (0=command, 1=data) |
| OLED RESET | 16 | GPIO |
| I2C SDA (UI board) | 38 | I2C1 |
| I2C SCL (UI board) | 39 | I2C1 |
| Debug UART TX | 20 | UART1 |
| PSRAM CS (APS6404) | 19 | XIP_CS1 (QMI M[1]) |
| Favorite button | 25 | GPIO (direct, no I2C) |

**Verified via RP2350 register reads:** GPIO14 = SPI1_SCLK (funcsel 0x01), GPIO15 = SPI1_TX (funcsel 0x01), GPIO38 = I2C1_SDA (funcsel 0x03), GPIO39 = I2C1_SCL (funcsel 0x03). `TBD_SPI = spi1` is correct.

---

## 2. SSD1309 OLED Display

### Specifications
- **Resolution:** 128×64 pixels, monochrome (1-bit per pixel)
- **Controller:** SSD1309 (Solomon Systech)
- **Interface:** PIO SPI at 10 MHz (SSD1309 datasheet maximum)
- **Physical size:** 2.4" diagonal — pixels are large and individually visible
- **Framebuffer:** 1024 bytes (128 columns × 8 pages, MSB = bottom pixel in page)
- **Addressing:** Horizontal addressing mode (0x20, 0x00)

### Initialization Sequence

```
0xFD 0x12   — Unlock command lock
0xAE        — Display OFF
0xD5 0xF0   — Set osc frequency to maximum (fastest refresh)
0xA8 0x3F   — MUX ratio = 64 (all 64 COM lines)
0xD3 0x00   — Display offset = 0
0x40        — Display start line = 0
0x20 0x00   — Horizontal addressing mode
0xA1        — Segment remap (horizontal flip — hardware is mounted 180°)
0xC0        — Normal COM scan direction (software Y-flip handles vertical)
0xDA 0x12   — COM pins: alternative, no L/R remap
0x81 0xDF   — Contrast = 0xDF (high, for OLED)
0xD9 0x82   — Pre-charge period
0xDB 0x34   — VCOMH deselect level
0xA4        — Entire display ON (follow RAM)
0xA6        — Normal display (not inverted)
0xAF        — Display ON
```

### Orientation Fix

The OLED module is physically mounted 180° rotated on the TBD-16 PCB. To correct:
- **Horizontal:** Segment remap `0xA1` (mirrors left↔right)
- **Vertical:** Software Y-flip in the rendering loop: `y = (DISPLAYHEIGHT - 1) - (p * 8 + b)`
- **COM scan:** `0xC0` (normal direction) — the Y-flip handles the inversion

**Note:** The separate lcdspi driver (used for UI mode, not Doom) uses `0xA1 + 0xC8` — a different orientation combo. The Doom driver's `0xA1 + 0xC0 + software Y-flip` is correct for the Doom rendering loop's page-walk order.

### GRAM Parking (command_park)

Before each frame write, the GRAM pointer must be reset:
```
0x21 0x00 0x7F   — Column address range: 0 to 127
0x22 0x00 0x07   — Page address range: 0 to 7
```

### SPI Speed

- **10 MHz:** Current setting, matches SSD1309 datasheet maximum (t_cycle ≥ 100 ns). Stable.
- **20-30 MHz:** Tested but failed on TBD-16 hardware (blank screen). Likely PCB trace/capacitance issue.

---

## 3. Display Rendering Pipeline

### Overview

The Doom engine renders at 128×64 (native OLED resolution). The display driver on core 1:

1. Reads the 128×64 8-bit paletted framebuffer
2. Looks up each palette index → luminance via `display_palette[256]`
3. Applies a shadow-lift LUT (`remap_lut[256]`) with configurable gamma
4. Dithers luminance to 1-bit using the selected method
5. Packs into SSD1309 page format (8 vertical pixels per byte)
6. Ships the 1024-byte frame to the display via SPI

### Dithering Research Framework

The project includes 15 compile-time switchable dithering methods for systematic A/B testing on the 128×64 SSD1309 OLED. Set `JTBD16_DITHER_MODE` in `jtbd16.h` to select:

| Mode | Name | Method | Strengths | Weaknesses |
|---:|---|---|---|---|
| 0 | `DITHER_ATKINSON` | Atkinson error diffusion | Clean surfaces, sharp edges, no tile moiré | Fewer grey levels, pattern "swimming" |
| 1 | `DITHER_BLUENOISE_STATIC` | Static blue noise threshold | Zero flicker, stable, simple | Only 2 grey levels per pixel |
| 2 | `DITHER_BLUENOISE_TEMPORAL` | 4-frame phase-shifted blue noise | 5 grey levels, broken moiré | Slight shimmer, noisy surfaces |
| 3 | `DITHER_3PASS_CONTRAST` | 3-pass contrast-weighted | ~7 grey levels (best gradients) | Flickers on SSD1309 (no VSYNC) |
| 4 | `DITHER_BLUENOISE_EDGE` | Blue noise + unsharp mask | Enhanced silhouettes | Risk of halos if strength too high |
| 5 | `DITHER_HYBRID_HUD` | Atkinson viewport + hard HUD | Crisp HUD text, smooth 3D | Visible transition at HUD boundary |
| 6 | `DITHER_FLOYD_STEINBERG` | Floyd-Steinberg error diffusion | Most grey levels (100% error), classic | "Wormy" serpentine patterns |
| 7 | `DITHER_SIERRA_LITE` | Sierra Lite error diffusion | Fast, nearly FS quality, less directional | Slightly fewer grey levels than FS |
| 8 | `DITHER_BN_FLOYD_STEINBERG` | FS + blue noise threshold | FS grey levels + organic BN texture | Tuning BN_MODULATION needed |
| 9 | `DITHER_BN_ATKINSON` | Atkinson + blue noise threshold | Clean Atkinson + BN breaks "swimming" | Tuning BN_MODULATION needed |
| 10 | `DITHER_BAYER4X4` | Bayer 4×4 ordered | Most stable in motion, no error propagation | Visible 4×4 crosshatch grid |
| 11 | `DITHER_BAYER8X8` | Bayer 8×8 ordered | Finer grid, 64 threshold levels | Pattern still visible but softer |
| 12 | `DITHER_SERPENTINE_FS` | Serpentine Floyd-Steinberg | Eliminates FS directional "wormy" patterns | Slightly more complex |
| 13 | `DITHER_JJN` | Jarvis-Judice-Ninke | Smoothest error diffusion, wide kernel | Slower (12 coefficients/pixel, /48) |
| 14 | `DITHER_STUCKI` | Stucki error diffusion | Slightly sharper than JJN, wide kernel | Slower (12 coefficients/pixel, /42) |

Default: mode 0 (Atkinson). **Modes 8-9 are BN-hybrid methods**. **Modes 10-11 are ordered (Bayer)** — best for motion stability. **Modes 12-14 are advanced error diffusion**.

### Tuning Parameters

All parameters are `#define`s in `jtbd16.h` with `#ifndef` guards — override via build flags or by editing the header.

| Parameter | Default | Range | Affects | Description |
|---|---|---|---|---|
| `JTBD16_DITHER_MODE` | 0 | 0-14 | All | Dithering method selection |
| `JTBD16_SHADOW_GAMMA` | 0 | 0-3 | All | LUT gamma curve: 0=pow(0.5) aggressive, 1=pow(0.625) moderate, 2=pow(0.8) mild, 3=linear |
| `JTBD16_DITHER_THRESHOLD` | 110 | 0-255 | 0, 1, 4-9, 12-14 | Black/white decision point. Lower = brighter output |
| `JTBD16_EDGE_STRENGTH` | 48 | 0-128 | 4 | Unsharp mask strength for edge boost |
| `JTBD16_HUD_Y_START` | 52 | 0-63 | 5 | First row of hard-threshold HUD region |
| `JTBD16_HUD_THRESHOLD` | 100 | 0-255 | 5 | Threshold for HUD/status bar region |
| `JTBD16_BN_MODULATION` | 48 | 0-128 | 8, 9 | Blue noise perturbation amplitude for hybrid modes |

**Quick override example:**
```
PLATFORMIO_BUILD_FLAGS="-DJTBD16_DITHER_MODE=1 -DJTBD16_SHADOW_GAMMA=2" pio run -e doom-tbd16
```

### Shadow-Lift LUT

The `remap_lut[256]` applies a configurable gamma curve with black point 12 and white point 232:

| Gamma | Exponent | Shadow detail | Use case |
|---:|---|---|---|
| 0 | pow(0.50) | Most aggressive — bright shadows | Dark corridor visibility |
| 1 | pow(0.625) | Moderate — balanced | General play |
| 2 | pow(0.80) | Mild — closer to original | Brighter maps |
| 3 | linear | No gamma — just BP/WP clamping | Reference baseline |

Generated by `gen_all_luts.py`. The LUT is shared across all dithering modes.

### Dithering History

| Approach | Result | Problem |
|---|---|---|
| Original 4-plane temporal (upstream) | Heavy flickering | SSD1309 no VSYNC, async scan tears |
| Bayer 4×4 ordered | Stable, 17 grey levels | Visible crosshatch grid at 2.4" |
| 16×16 blue noise (single frame) | Natural grain | Only 2 grey levels |
| 2-frame complementary blue noise | Better grey levels | Worm/moiré from 16×16 tile repeats |
| 4-frame phase-shifted blue noise | 5 grey levels | Noisy surfaces, checkerboard on walls |
| Atkinson error diffusion | Clean surfaces, sharp edges | Too dark in corridors |
| Floyd-Steinberg error diffusion | Most grey levels | Wormy serpentine patterns |
| BN-modulated error diffusion | Best of ED + BN | Tuning of BN_MODULATION needed |
| Bayer 4×4 ordered | Most stable in motion, no cascading | Visible crosshatch grid at 2.4" |
| Bayer 8×8 ordered | Finer grid than 4×4, 64 levels | Pattern still visible but softer |
| Serpentine Floyd-Steinberg | Eliminates FS directional bias | — |
| JJN / Stucki | Smoothest ED, 3-row wide kernel | Division by 48/42 per pixel |
| **Research framework (current)** | 15 methods + tunable brightness | Systematic comparison possible |

---

## 4. Build System

### PlatformIO Setup

The project uses PlatformIO with Pico SDK framework (not Arduino). Key config:

```ini
[env:doom-tbd16]
platform = https://github.com/maxgerhardt/platform-raspberrypi.git
board = generic_rp2350
framework = picosdk
board_build.core = earlephilhower
upload_protocol = cmsis-dap
```

### Build Scripts

- **`doom_build.py`** — Pre-build script that adds all rp2040-doom source files and include paths. Replaces the upstream CMake build. Also configures TinyUSB headers needed by pico_stdio (even though USB stdio is disabled).

### PlatformIO Conventions

- `PIO_STDIO_UART` is NOT a typo — it's PlatformIO's convention, remapped to `LIB_PICO_STDIO_UART` by the picosdk.py builder
- `PIO_STDIO_USB` is the default if NO `PIO_STDIO_*` flags exist — must have `PIO_STDIO_UART` to prevent USB auto-enable
- TinyUSB newer SDK needs `CFG_TUD_CDC_RX_BUFSIZE` / `CFG_TUD_CDC_TX_BUFSIZE` defines

### Flashing

**Recommended — CMSIS-DAP debug probe:**
```bash
./flash.sh all        # firmware + WAD via debug probe
./flash.sh uf2        # combined UF2 via picotool USB
pio run -t upload     # firmware only (no WAD!)
```

**UF2 drag-and-drop** is not currently working — see **Section 9** for research details. Use `picotool` or the debug probe instead.

**Never use:**
- `idf.py flash` (ESP32 tool, causes boot loops on ESP-based boards)
- `esptool.py` (ESP32 tool, not applicable to RP2350)

### Command Reference

All commands run from the project root.

#### Build only (no flash)

```bash
# Default config (mode 0, gamma 0, threshold 110)
pio run -e doom-tbd16

# Clean build
pio run -e doom-tbd16 -t clean && pio run -e doom-tbd16
```

#### Build and flash

```bash
# Default config
pio run -e doom-tbd16 -t upload
```

#### Override dithering mode

```bash
# Mode 1 — static blue noise
PLATFORMIO_BUILD_FLAGS="-DJTBD16_DITHER_MODE=1" pio run -e doom-tbd16 -t upload

# Mode 2 — temporal blue noise
PLATFORMIO_BUILD_FLAGS="-DJTBD16_DITHER_MODE=2" pio run -e doom-tbd16 -t upload

# Mode 3 — 3-pass contrast
PLATFORMIO_BUILD_FLAGS="-DJTBD16_DITHER_MODE=3" pio run -e doom-tbd16 -t upload

# Mode 4 — blue noise + edge boost
PLATFORMIO_BUILD_FLAGS="-DJTBD16_DITHER_MODE=4" pio run -e doom-tbd16 -t upload

# Mode 5 — hybrid Atkinson + HUD
PLATFORMIO_BUILD_FLAGS="-DJTBD16_DITHER_MODE=5" pio run -e doom-tbd16 -t upload

# Mode 6 — Floyd-Steinberg
PLATFORMIO_BUILD_FLAGS="-DJTBD16_DITHER_MODE=6" pio run -e doom-tbd16 -t upload

# Mode 7 — Sierra Lite
PLATFORMIO_BUILD_FLAGS="-DJTBD16_DITHER_MODE=7" pio run -e doom-tbd16 -t upload

# Mode 8 — BN-modulated Floyd-Steinberg (hybrid)
PLATFORMIO_BUILD_FLAGS="-DJTBD16_DITHER_MODE=8" pio run -e doom-tbd16 -t upload

# Mode 9 — BN-modulated Atkinson (hybrid)
PLATFORMIO_BUILD_FLAGS="-DJTBD16_DITHER_MODE=9" pio run -e doom-tbd16 -t upload

# Mode 10 — Bayer 4×4 ordered
PLATFORMIO_BUILD_FLAGS="-DJTBD16_DITHER_MODE=10" pio run -e doom-tbd16 -t upload

# Mode 11 — Bayer 8×8 ordered
PLATFORMIO_BUILD_FLAGS="-DJTBD16_DITHER_MODE=11" pio run -e doom-tbd16 -t upload

# Mode 12 — Serpentine Floyd-Steinberg
PLATFORMIO_BUILD_FLAGS="-DJTBD16_DITHER_MODE=12" pio run -e doom-tbd16 -t upload

# Mode 13 — Jarvis-Judice-Ninke
PLATFORMIO_BUILD_FLAGS="-DJTBD16_DITHER_MODE=13" pio run -e doom-tbd16 -t upload

# Mode 14 — Stucki
PLATFORMIO_BUILD_FLAGS="-DJTBD16_DITHER_MODE=14" pio run -e doom-tbd16 -t upload
```

#### Override gamma curve

```bash
# Gamma 0 — aggressive (pow 0.5, brightest shadows)
PLATFORMIO_BUILD_FLAGS="-DJTBD16_SHADOW_GAMMA=0" pio run -e doom-tbd16 -t upload

# Gamma 1 — moderate (pow 0.625)
PLATFORMIO_BUILD_FLAGS="-DJTBD16_SHADOW_GAMMA=1" pio run -e doom-tbd16 -t upload

# Gamma 2 — mild (pow 0.8)
PLATFORMIO_BUILD_FLAGS="-DJTBD16_SHADOW_GAMMA=2" pio run -e doom-tbd16 -t upload

# Gamma 3 — linear (no gamma, reference)
PLATFORMIO_BUILD_FLAGS="-DJTBD16_SHADOW_GAMMA=3" pio run -e doom-tbd16 -t upload
```

#### Combined overrides

```bash
# Multiple flags — mode + gamma + threshold
PLATFORMIO_BUILD_FLAGS="-DJTBD16_DITHER_MODE=1 -DJTBD16_SHADOW_GAMMA=2 -DJTBD16_DITHER_THRESHOLD=100" \
  pio run -e doom-tbd16 -t upload

# Mode 4 with custom edge strength
PLATFORMIO_BUILD_FLAGS="-DJTBD16_DITHER_MODE=4 -DJTBD16_EDGE_STRENGTH=64" \
  pio run -e doom-tbd16 -t upload

# Mode 5 with custom HUD boundary
PLATFORMIO_BUILD_FLAGS="-DJTBD16_DITHER_MODE=5 -DJTBD16_HUD_Y_START=48 -DJTBD16_HUD_THRESHOLD=90" \
  pio run -e doom-tbd16 -t upload

# Mode 8 with custom BN modulation strength
PLATFORMIO_BUILD_FLAGS="-DJTBD16_DITHER_MODE=8 -DJTBD16_BN_MODULATION=64" \
  pio run -e doom-tbd16 -t upload

# Mode 9 with low modulation (closer to pure Atkinson)
PLATFORMIO_BUILD_FLAGS="-DJTBD16_DITHER_MODE=9 -DJTBD16_BN_MODULATION=24" \
  pio run -e doom-tbd16 -t upload
```

#### Regenerate gamma LUTs

```bash
python3 gen_all_luts.py > /tmp/luts.c
# Then paste the output into i_video.c's gamma LUT section
```

#### Monitor serial output

```bash
pio device monitor -e doom-tbd16
```

---

## 5. Memory Layout

### SRAM (RP2350)
- `SRAM4_BASE = 0x20040000` (same address as RP2040)
- Doom's zone allocator uses `__HeapLimit` to `SRAM4_BASE` (~170KB available)
- `DOOM_TINY` is designed for ~40KB — 170KB is plenty
- Display frame buffers: 4 × 1024 bytes = 4KB static allocation on core 1's stack

### Flash (XIP-mapped)

The RP2350 memory-maps SPI flash via XIP (Execute In Place) starting at `XIP_BASE = 0x10000000`. The firmware occupies ~250 KB starting at `0x10000000`.

> **Note:** The WAD file is NOT stored in flash. It is loaded from the SD card into PSRAM at boot. See the PSRAM section below.

#### WAD Format (WHX)

The `doom1.whx` file is a compressed Doom WAD variant produced by `whd_gen`. It uses a packed lump table where each `lumpinfo_t` is a single `uint32_t` (offset only). Valid magic bytes: `IWAD`, `IWHD`, or `IWHX`.

#### WAD Loading — SD Card to PSRAM

The WAD file is loaded from the SD card into PSRAM at boot by `sd_wad_loader.c`:

1. Initializes PSRAM (8 MB APS6404 on QMI CS1)
2. Mounts FAT filesystem via Petit FatFS on SPI0
3. Opens `/DATA/DOOM1.WHX` and streams it into PSRAM at `0x11000000`
4. Releases SPI0 pins for P4 link use
5. Validates WAD magic bytes in PSRAM

After loading, the WAD is accessed via direct memory-mapped pointers into PSRAM.

> **Historical note:** An earlier approach (`append_wad_uf2.py`) embedded the WAD into the firmware UF2 at flash address `0x10040000`. This was abandoned because UF2 drag-and-drop flashing of the combined image never worked reliably. See section 9 for the research log.

#### Relevant Build Flags

| Flag | Value | Purpose |
|---|---|---|
| `USE_SD_WAD` | `1` | Enable SD card WAD loading |
| `USE_WHD` | `1` | Enable compressed WHD/WHX format support |
| `USE_MEMMAP_ONLY` | `1` | Force all lump access through memory pointers |

### PSRAM (APS6404, 8 MB, QPI on QMI CS1)

The TBD-16 has an APS6404 8 MB PSRAM on GPIO19, accessed via the RP2350's QMI peripheral (CS1). After initialization, PSRAM is memory-mapped at `0x11000000` (XIP_BASE + 0x01000000) and is directly readable/writable as normal memory.

The WAD file (~500 KB) is loaded from SD card into PSRAM at boot by `sd_wad_loader.c`.

#### PSRAM Init — Critical RP2350 Gotchas

`psram_init.c` is written with **zero SDK headers** (no `hardware/gpio.h`, `hardware/clocks.h`, etc.) because SDK headers pull in `stdbool.h` via `pico/types.h`, which causes the PSRAM init to hang on RP2350.

Two RP2350-specific issues required careful handling:

1. **Pad Isolation (ISO bit):** On RP2350 (unlike RP2040), every GPIO pad starts with `ISO=1` (bit 8 of `PADS_BANK0` register) after power-on reset. This electrically disconnects the pad. The SDK's `gpio_set_function()` clears ISO automatically, but with raw register access it must be done explicitly:
   ```c
   PAD_SET(pin) = PAD_IE;              // enable input
   PAD_CLR(pin) = PAD_OD | PAD_ISO;    // enable output, remove isolation
   ```

2. **QMI M[1] Stale State:** The QMI peripheral is NOT reset by SYSRESETREQ (debug-probe reset). After a warm reset, M[1] retains its rfmt/rcmd/wfmt/wcmd from the previous boot. If GPIO19 is connected to XIP_CS1 while M[1] has valid config, QMI may immediately start a stale transaction that blocks M[0] (flash), freezing the CPU. Fix: clear M[1] registers **before** switching GPIO19 to `FUNC_XIP_CS1`.

#### PSRAM Register Reference

| Register | Address |
|---|---|
| QMI_BASE | `0x400d0000` |
| PADS_BANK0_BASE | `0x40038000` |
| GPIO19 pad reg | `0x40038050` (BASE + 0x04 + 19×4) |
| GPIO19 IO_BANK0 CTRL | `0x4002809C` (BASE + 19×8 + 4) |
| FUNC_XIP_CS1 | funcsel = 9 |

---

## 6. I2C Input System

### STM32 UI Board Protocol
- **Bus:** I2C1 at 400 kHz
- **Address:** 0x42
- **Data format:** REV_C = 48 bytes, REV_B = 42 bytes
- **Button bits:** At byte offset 34 (`mcl_btns`), 16-bit bitfield

### Button Bit Indices

| Bit | Physical Button | Doom Mapping |
|---|---|---|
| 0 | Left | Turn left |
| 1 | Down | Move backward |
| 2 | Right | Turn right |
| 3 | Up | Move forward |
| 4 | FUNC5 (A) | Fire |
| 5 | FUNC6 (B) | Use / Open doors |
| 6 | MASTER (X) | Strafe modifier |
| 7 | SOUND (Y) | Run modifier |
| 8 | Play | Menu / Pause |
| 9 | Record | Toggle automap |
| 10 | Shift 1 | Previous weapon |
| 11 | Shift 2 | Next weapon |

### STM32 Reset

The RP2350 controls the STM32 reset via GPIO40 (100ms LOW pulse). The I2C bus pins and pull-ups must be configured **before** releasing STM32 from reset — otherwise the STM32 may see bus glitches during its initialization.

The init sequence in `i2c_ui_init()` is:
1. Configure GPIO38/39 as I2C1 with internal pull-ups
2. Initialize I2C1 at 400 kHz
3. Assert STM32 reset (GPIO40 LOW for 100ms)
4. Release reset, wait 200ms for STM32 to boot
5. Begin polling

### Boot Debug Display

The firmware includes OLED-based boot stage diagnostics (digits 0-9 on page 0, hex values on pages 1-2). These are **disabled by default** but can be re-enabled:

```c
// In jtbd16.h or via build flags:
#define JTBD16_BOOT_DEBUG 1
```

Or via command line:
```bash
PLATFORMIO_BUILD_FLAGS="-DJTBD16_BOOT_DEBUG=1" pio run -e doom-tbd16 -t upload
```

The debug display shows boot progress stages 1-9 and hex checkpoint values. The functions (`debug_show_stage`, `debug_show_hex`, `debug_show_hex2`) are defined in `i_video.c` and compile to no-ops when `JTBD16_BOOT_DEBUG` is 0.

---

## 7. Reference Firmware Notes

From analyzing the original TBD-16 firmware (`tbd-pico-seq3`):

- Uses **software SPI** (bit-banged via Arduino SoftwareSPI) for OLED — not because HW SPI can't work, just a framework choice. The Doom port uses hardware SPI1.
- OLED reset sequence: HIGH → LOW → HIGH with 10ms delays (30ms total)
- SSD1309 init uses contrast `0xDF`, clock divider `0xA0` (vs Doom's `0xF0` for max refresh)
- The original firmware drives its SPI buses (to ESP32-P4) at 30 MHz — but that's a different SPI peripheral than the OLED

---

## 8. Utility Scripts

| Script | Purpose |
|---|---|
| `gen_blue_noise2.py` | Generate 16×16 void-and-cluster blue noise texture (pure Python, no scipy) |
| `gen_remap_lut.py` | Generate a single shadow-lift gamma LUT (original) |
| `gen_all_luts.py` | Generate all 4 gamma LUT variants for the dithering framework |
| `gen_gamma.py` | Generate gamma correction tables |
| `gen_lut.py` | General LUT generation utilities |
| `flash.sh` | Multi-mode flash script (debug probe / picotool / UF2) |
| `split_uf2.py` | Split combined UF2 into firmware-only and WAD-only files |
| `check_uf2.py` | Validate UF2 file structure |
| `compare_uf2.py` | Compare two UF2 files for differences |
| `extract_fw.py` | Extract firmware from flash dumps |
| `compare_boot.py` | Compare boot stage binaries |

---

## 9. UF2 Flashing Research & Status (Historical)

> **Note:** This section documents the failed WAD-embed approach. The current firmware loads the WAD from SD card into PSRAM at boot, so UF2 only needs to contain the firmware (~500 KB). Plain BOOTSEL drag-and-drop works for firmware-only UF2 files.

### Status: WAD-in-UF2 Drag-and-Drop Was Abandoned

**TLDR:** Drag-and-drop flashing of the combined firmware+WAD UF2 via RP2350 BOOTSEL mass-storage mode **does not work** despite extensive investigation. The `cp` command to `/Volumes/RP2350/` always exits with code 1 and the device fails to boot.

**Working alternatives:**
- `picotool load --ignore-partitions -v -x firmware.uf2` — **WORKS** (USB BOOTSEL mode)
- CMSIS-DAP debug probe via `./flash.sh all` — **WORKS** (SWD)

### The Core Problem

The combined firmware+WAD UF2 is ~4 MB (~8018 blocks), spanning addresses `0x10000000`–`0x101F7800`. The RP2350 BOOTSEL mass-storage bootloader applies restrictions on which UF2 blocks it will accept. Multiple factors contribute:

1. **Partition boundaries:** By default, the bootloader assumes a single small firmware partition. UF2 blocks targeting addresses outside known partitions are silently dropped.
2. **Family ID routing:** The bootloader uses the UF2 family ID field (offset 28) to route blocks to partitions. If a partition's family list doesn't match the block's family, that block is ignored.
3. **IMAGE_DEF requirement:** The bootloader requires an IMAGE_DEF/vector table to accept a UF2 and trigger reboot. Data-only UF2s are silently discarded.

### Approaches Tried (All Failed for Drag-and-Drop)

#### Attempt 1: Default Build (RP2350-ARM-S Family)

PlatformIO produces firmware blocks with `RP2350_ARM_S` family (`0xE48BFF59`). WAD blocks appended with the same family. Expected the bootloader to write all blocks within its default partition range.

**Result:** WAD blocks silently dropped — they target addresses beyond the default partition.

#### Attempt 2: Separate WAD-Only UF2

Created a WAD-only UF2 to flash independently (using `split_uf2.py`).

**Result:** Rejected — no IMAGE_DEF/vector table means the bootloader ignores the entire UF2.

#### Attempt 3: Partition Table with Both Partitions (Same Family)

Created `partition_table.json` with two partitions — firmware (256K) and WAD (15872K) — both using `rp2350-arm-s` family. Embedded PT in firmware ELF via `picotool partition create`.

```json
{
  "partitions": [
    { "name": "Doom Firmware", "start": 0, "size": "256K", "families": ["rp2350-arm-s"] },
    { "name": "WAD Data", "start": "256K", "size": "15872K", "families": ["rp2350-arm-s"],
      "no_reboot_on_uf2_download": true }
  ]
}
```

**Result:** Failed. Per Pi engineer **kilograham** on the Raspberry Pi forums: when two partitions share the same family ID, the bootloader cannot distinguish which partition a block belongs to.

#### Attempt 4: Partition Table with DATA Family for WAD

Changed WAD partition to use `data` family (`0xE48BFF58`), WAD blocks stamped with DATA family.

**Result:** Failed. The `cp` to `/Volumes/RP2350/` still exits with code 1.

#### Attempt 5: ABSOLUTE Family for WAD, No WAD Partition

Based on advice from Pi engineer **will-v-pi** (forum post t=388069): use the "absolute" UF2 family (`0xE48BFF57`) to write to stated flash addresses bypassing partition routing. Removed the WAD partition, kept only firmware partition, WAD blocks stamped ABSOLUTE. Ensured WAD blocks come first in the file.

**Result:** Failed.

#### Attempt 6: All Blocks ABSOLUTE Family with Partition Table

Re-stamped ALL blocks (firmware + WAD) to ABSOLUTE family. Kept a single firmware partition with `"start": 0` to avoid the flip-flop bug (see below).

**Result:** Failed.

#### Attempt 7: All Blocks ABSOLUTE Family, No Partition Table (Current)

Simplest possible approach: no partition table at all, all ~8018 blocks stamped with ABSOLUTE family (`0xE48BFF57`). The build script (`append_wad_uf2.py`) re-stamps firmware blocks from `RP2350_ARM_S` to `ABSOLUTE` and appends WAD blocks also as `ABSOLUTE`.

**Result:** Failed. Verified via `picotool load --ignore-partitions` that the UF2 content is valid (device boots and runs correctly when flashed this way), but drag-and-drop still fails.

### Key Research Findings

#### Source 1: Raspberry Pi Forums (t=388069)

Pi engineer **will-v-pi** on using multiple UF2 families:
> "Use the absolute UF2 family to target absolute flash addresses, bypassing partition routing."

Pi engineer **kilograham** warned:
> "Concatenating multiple UF2s with different families is not strictly supported — the host OS may interleave sectors from different parts of the file, and the bootloader processes them in arrival order."

**Implication:** Mixed-family UF2 files (e.g., firmware as `rp2350-arm-s` + WAD as `absolute`) may fail because macOS's FAT write implementation could interleave the 512-byte sectors unpredictably.

#### Source 2: pico-sdk GitHub Issue #1882

Pi engineer **will-v-pi** explained the **partition table flip-flop bug**: when `picotool partition create` embeds the PT, it places the PT section at the end of the firmware's flash region (e.g., `0x1003D8AC`). If partition 0 starts at the default address (`0x10002000`), the PT itself falls _outside_ partition 0's range. On first UF2 download, the PT writes correctly. On second download, the bootloader reads the now-existing PT, sees that the PT's own address is outside partition 0, and refuses to overwrite it — leaving stale PT data.

**Fix:** Add `"start": 0` to partition 0 in the JSON so the partition covers the entire range from `0x10000000` including the PT section. This was applied in our attempts but did not resolve the drag-and-drop issue.

#### Source 3: Pico W Reference Implementation (wifi_pt.json)

The Pico W SDK uses a partition table with different families per partition:
- Partition 0 (code): family `rp2350-arm-s`
- Partition 1 (wifi firmware): family `cyw43-firmware` (`0xE48BFF55`)

This is the canonical example of multi-partition UF2 — but the Pico W's second partition is much smaller and uses purpose-built tooling.

#### Source 4: picotool partition create

Command syntax (non-obvious argument order):
```
picotool partition create <json_input> <elf_output> -t elf <bootloader_elf_input> [--abs-block]
```

- Argument 2 is the **output** ELF (with embedded PT)
- The `-t elf` input is the **source** firmware ELF
- `--abs-block` adds an absolute block at `0x10FFFF00` for RP2350 errata E9

**Gotcha:** If you convert the original ELF (not the output) to UF2, the PT is missing. And if you pass the same file as both input and output, the result is undefined.

### UF2 Family ID Reference

| Family ID | Constant | Hex | Purpose |
|---|---|---|---|
| Absolute | `ABSOLUTE_FAMILY_ID` | `0xE48BFF57` | Write to stated address, bypass partition routing |
| Data | `DATA_FAMILY_ID` | `0xE48BFF58` | Generic data partitions |
| RP2350 ARM-S | `RP2350_FAMILY_ID` | `0xE48BFF59` | RP2350 ARM Secure firmware (PlatformIO default) |
| RP2350 ARM-NS | — | `0xE48BFF5A` | RP2350 ARM Non-Secure firmware |
| RP2350 RISC-V | — | `0xE48BFF5B` | RP2350 RISC-V firmware |

Source: `~/.platformio/packages/framework-picosdk/src/common/boot_uf2_headers/include/boot/uf2.h`

### Current Build Pipeline

The `append_wad_uf2.py` post-build script (removed — documented here for reference):

1. PlatformIO builds `firmware.elf` → converts to `firmware.uf2` (family `RP2350_ARM_S`)
2. Post-build reads `firmware.uf2`, re-stamps all firmware blocks to `ABSOLUTE_FAMILY_ID`
3. Reads `data/doom1.whx`, creates WAD UF2 blocks at `TINY_WAD_ADDR` (`0x10040000`) with `ABSOLUTE_FAMILY_ID`
4. Concatenates firmware + WAD blocks, fixes sequence numbers, writes combined `firmware.uf2`
5. Result: ~8018 blocks, all `ABSOLUTE` family, addresses `0x10000000`–`0x101F7800`, ~4.1 MB

> This approach was abandoned in favor of SD card WAD loading.

### Flashing Methods

#### Method 1: CMSIS-DAP Debug Probe (Recommended for Development)

Direct SWD flashing — no partition restrictions, flashes firmware and WAD independently.

```bash
./flash.sh all        # Flash both firmware + WAD (default)
./flash.sh firmware   # Flash firmware only (~250K, ~4 seconds)
./flash.sh wad        # Flash WAD only (~1.8M, ~23 seconds)
```

Or via PlatformIO:
```bash
pio run -t upload     # Flash firmware only (no WAD!)
```

**Note:** `pio run -t upload` only flashes the firmware ELF. It does NOT include the WAD data. Use `./flash.sh all` for a complete flash.

Uses OpenOCD with CMSIS-DAP interface:
```bash
openocd -f interface/cmsis-dap.cfg -f target/rp2350.cfg -c "adapter speed 5000" \
  -c "init" -c "reset halt" \
  -c "flash write_image erase firmware.elf" \
  -c "reset run" -c "shutdown"
```

#### Method 2: picotool via USB (Recommended for Production)

Requires device in BOOTSEL mode (hold BOOT, press RESET). Flashes the combined firmware+WAD UF2:

```bash
./flash.sh uf2
# or directly:
picotool load --ignore-partitions -v -x firmware.uf2
```

The `--ignore-partitions` flag bypasses partition validation, allowing the entire combined UF2 to be written regardless of partition table state.

#### Method 3: UF2 Drag-and-Drop (NOT WORKING)

⚠️ **This method does not currently work.** See "Approaches Tried" above.

The file `firmware.uf2` is a valid combined UF2 and flashes correctly via picotool, but fails when copied to the RP2350 BOOTSEL mass-storage volume:

```bash
# This fails with exit code 1:
cp .pio/build/doom-tbd16/firmware.uf2 /Volumes/RP2350/
```

The volume name is **RP2350** (not RPI-RP2, which is the Pico 1 volume name).

### UF2 File Format Reference

Each UF2 block is exactly 512 bytes:

| Offset | Size | Field | Value |
|--------|------|-------|-------|
| 0 | 4 | Magic 0 | `0x0A324655` ("UF2\n") |
| 4 | 4 | Magic 1 | `0x9E5D5157` |
| 8 | 4 | Flags | `0x00002000` (family ID present) |
| 12 | 4 | Target Address | `0x10000000` + offset |
| 16 | 4 | Payload Size | 256 (max) |
| 20 | 4 | Block Number | Sequential (0-based) |
| 24 | 4 | Total Blocks | Total count across all blocks |
| 28 | 4 | Family ID | `0xE48BFF57` (Absolute) |
| 32 | 256 | Data Payload | Firmware or WAD bytes |
| 288 | 220 | Padding | Zeros |
| 508 | 4 | Magic End | `0x0AB16F30` |

### Utility Scripts

| Script | Purpose |
|---|---|
| `flash.sh` | Multi-mode flash script (debug probe, picotool, UF2) |
| `split_uf2.py` | Splits combined UF2 into firmware-only and WAD-only files |
| `check_uf2.py` | Validates UF2 file structure and block integrity |
| `compare_uf2.py` | Compares two UF2 files for differences |

### Tool Versions

| Tool | Version | Path |
|---|---|---|
| picotool | 5.140200.250530 | `~/.platformio/packages/tool-picotool-rp2040-earlephilhower/picotool` |
| OpenOCD | 0.12.0+dev | `~/.platformio/packages/tool-openocd-rp2040-earlephilhower/bin/openocd` |
| Pico SDK | 2.2.0 | `~/.platformio/packages/framework-picosdk/` |

### Unexplored Avenues

The following ideas have NOT been tested and may be worth investigating:

1. **Firmware-only UF2 via drag-and-drop:** Test whether even a small firmware-only UF2 (no WAD, ~250K) works via mass storage. If not, the issue is not related to file size or WAD data at all — it could be a board-level USB or bootloader issue specific to the TBD-16 hardware.
2. **File size limit:** The combined UF2 is ~4 MB. The RP2350 mass-storage bootloader may have undocumented transfer size limits.
3. **macOS-specific behavior:** macOS `cp` to FAT volumes has known quirks (e.g., creating `._` resource fork files). Try from Linux or Windows.
4. **Explicit eject:** After `cp`, run `diskutil eject /Volumes/RP2350` to ensure the FAT write completes before the device disconnects.
5. **Board-specific USB:** The TBD-16 is a custom board, not a standard Pico 2. Its USB routing or power may affect BOOTSEL behavior.
6. **picotool partition info on device:** Check what partition state the RP2350 boot ROM actually sees after various flash methods.

### Troubleshooting

| Symptom | Cause | Fix |
|---|---|---|
| `cp firmware.uf2 /Volumes/RP2350/` exits with code 1 | **Unsolved** — see research above | Use `picotool load --ignore-partitions -v -x firmware.uf2` instead |
| UF2 drag-and-drop: drive doesn't unmount after copy | UF2 has no IMAGE_DEF (e.g., WAD-only UF2) | Use combined firmware+WAD UF2, not WAD-only |
| `picotool partition create` succeeds but PT missing in UF2 | Using wrong ELF for `uf2 convert` (the unmodified one) | Convert the OUTPUT ELF from `partition create`, not the original |
| Build shows `[PT] WARNING: failed to embed partition table` | picotool not found or wrong arg order | Check picotool path; ensure `--abs-block` comes after bootloader ELF arg |
| `picotool partition info` → "No accessible devices" | Device not in BOOTSEL mode, or running application | Hold BOOT + press RESET to enter BOOTSEL |

---

## 10. Known Issues & Future Work

- **UF2 drag-and-drop (WAD-in-UF2):** The historical approach of embedding WAD in the UF2 and drag-dropping never worked. See **Section 9** for research. Current approach (SD card) bypasses this entirely. Firmware-only UF2 drag-and-drop works.
- **SPI speed:** 10 MHz works (SSD1309 datasheet max). 20-30 MHz causes blank screen on current hardware — likely PCB trace characteristics.
- **UART debug:** UART1 TX on GPIO20 is wired to the debug probe but currently produces no output. Suspected physical wire disconnection on the 4th SWD cable.
