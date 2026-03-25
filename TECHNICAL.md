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

The Doom engine renders to a 320×200 8-bit paletted framebuffer. The display driver on core 1:

1. Reads the 320×200 framebuffer (only the first 128 columns, 64 rows are visible)
2. Looks up each palette index → luminance via `display_palette[256]`
3. Optionally applies a shadow-lift LUT (`remap_lut[256]`, gamma 0.625)
4. Dithers luminance to 1-bit using blue noise thresholds
5. Packs 8 vertical pixels into one byte (SSD1309 page format)
6. Ships the 1024-byte frame to the display via SPI

### Evolution of Dithering Approaches

| Approach | Result | Problem |
|---|---|---|
| **Original 4-plane temporal** (upstream) | Heavy flickering, brightness bands | SSD1309 has no VSYNC; async scan tears between planes of vastly different brightness |
| **Bayer 4×4 ordered** | Stable, 17 grey levels | Visible crosshatch grid pattern at 2.4" pixel size |
| **16×16 blue noise** (single frame) | Natural grain, no grid | Only 2 grey levels (on/off), no temporal integration |
| **2-frame complementary blue noise** | Better: `lum > thr` and `lum > ~thr` | Still visible worm/moiré pattern from 16×16 tile repeats |
| **4-frame phase-shifted blue noise** (current) | Best: 5 grey levels, reduced moiré | Optimal for this hardware |

### Current Implementation: 4-Frame Phase-Shifted Blue Noise

```c
// Phase offsets — coprime to 16 for maximum spatial diversity
static const uint8_t phase_dx[4] = {0, 7, 3, 11};
static const uint8_t phase_dy[4] = {0, 3, 11,  7};

// For each pixel, each frame samples blue_noise at a different offset:
if (lum > blue_noise[(y + phase_dy[f]) & 15][(x + phase_dx[f]) & 15])
    pixel = ON;
```

**Why phase offsets work:**
- The 16×16 blue noise tile repeats every 16 pixels, which can alias with Doom's repeating wall textures (also powers of 2)
- Shifting the sampling point by amounts coprime to 16 (7, 3, 11) means each frame's dither pattern is maximally different
- The eye integrates all 4 → moiré appears as subtle texture rather than visible worms

**Frame cycling:** At 8 MHz SPI, each frame takes ~1ms to write. All 4 frames cycle at ~250 Hz — well above the ~60 Hz flicker fusion threshold.

### Blue Noise Texture

Generated by `gen_blue_noise2.py` — a pure Python void-and-cluster algorithm (no scipy dependency). Parameters: 16×16 grid, sigma=1.5, seed=42. Produces 256 unique threshold values ensuring uniform distribution.

### Shadow-Lift LUT

The `remap_lut[256]` applies a pow(0.625) gamma curve that:
- Lifts dark values (Doom corridors are very dark)
- Maps palette index 0-18 → 0 (true black preserved for screen edges)
- Maps palette index 228-255 → 255 (full white for bright areas)
- Controlled by `JTBD16_SHADOW_LIFT` define in `jtbd16.h`

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
| 5 | FUNC6 (B) | Use |
| 6 | MASTER (X) | Run |
| 7 | SOUND (Y) | Strafe |
| 8 | Play | — |
| 9 | Record | — |
| 10 | Shift 1 | — |
| 11 | Shift 2 | — |

### STM32 Reset

The ESP32-P4 can reset the STM32 via GPIO40 (100ms LOW pulse). The RP2350 does not control this — it just reads I2C after the STM32 is already running.

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
| `gen_remap_lut.py` | Generate the shadow-lift gamma LUT |
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
- **Grey levels:** 4 temporal frames give 5 levels. More frames would give more levels but the rendering loop already takes significant CPU time on core 1.
- **Moiré:** Phase-shifted offsets reduce but don't eliminate moiré against certain Doom texture frequencies. A larger blue noise tile (32×32 or 64×64) would help but costs more flash and cache pressure.
- **WAD loading:** Currently uses the embedded shareware `doom1.whx`. SD card loading would allow full Doom and custom WADs.
