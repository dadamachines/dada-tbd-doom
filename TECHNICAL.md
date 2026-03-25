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
| Favorite button | 25 | GPIO (direct, no I2C) |

**Verified via RP2350 register reads:** GPIO14 = SPI1_SCLK (funcsel 0x01), GPIO15 = SPI1_TX (funcsel 0x01), GPIO38 = I2C1_SDA (funcsel 0x03), GPIO39 = I2C1_SCL (funcsel 0x03). `TBD_SPI = spi1` is correct.

---

## 2. SSD1309 OLED Display

### Specifications
- **Resolution:** 128×64 pixels, monochrome (1-bit per pixel)
- **Controller:** SSD1309 (Solomon Systech)
- **Interface:** SPI1 at 8 MHz
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

- **8 MHz:** Known stable, ~1ms per 1024-byte frame write
- **20-30 MHz:** The SSD1309 datasheet allows higher speeds, but failed on TBD-16 hardware (blank screen). Likely PCB trace/capacitance issue. Stick with 8 MHz.

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
- **`append_wad_uf2.py`** — Post-build script that appends the WAD data.

### PlatformIO Conventions

- `PIO_STDIO_UART` is NOT a typo — it's PlatformIO's convention, remapped to `LIB_PICO_STDIO_UART` by the picosdk.py builder
- `PIO_STDIO_USB` is the default if NO `PIO_STDIO_*` flags exist — must have `PIO_STDIO_UART` to prevent USB auto-enable
- TinyUSB newer SDK needs `CFG_TUD_CDC_RX_BUFSIZE` / `CFG_TUD_CDC_TX_BUFSIZE` defines

### Flashing

**Always use CMSIS-DAP debug probe:**
```bash
pio run -t upload
```

**Never use:**
- UF2 drag-and-drop (WAD data won't be placed correctly)
- `idf.py flash` (wrong platform, causes boot loops on ESP-based boards)
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

### Flash
- Firmware + doom engine: ~250KB
- `doom1.whx` (compressed WAD): appended after firmware by `append_wad_uf2.py`

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
| `check_uf2.py` | Validate UF2 file structure |
| `compare_uf2.py` | Compare two UF2 files for differences |
| `extract_fw.py` | Extract firmware from flash dumps |
| `compare_boot.py` | Compare boot stage binaries |

---

## 9. Known Issues & Future Work

- **Audio:** Not implemented. Sound stubs exist (`i_picosound_stub.c`) but no I2S output to the audio codec.
- **SPI speed:** 8 MHz works, 20-30 MHz causes blank screen on current hardware. May be fixable with better SPI signal integrity or slower ramp rates.
- **UART debug:** UART1 TX on GPIO20 is wired to the debug probe but currently produces no output despite correct initialization. Suspected physical wire disconnection on the 4th SWD cable. The 3 SWD wires (SWDIO, SWCLK, GND) work for CMSIS-DAP flashing.
- **WAD loading:** Currently uses the embedded shareware `doom1.whx`. SD card loading would allow full Doom and custom WADs.
