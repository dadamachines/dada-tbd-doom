# dadamachines TBD App Template

The official starting point for building RP2350 apps for the [dadamachines TBD-16](https://dadamachines.com) audio DSP platform.

This template provides the core libraries and project structure for creating custom instruments, effects, sequencers, and utilities that run on the TBD-16's RP2350 co-processor — communicating with the ESP32-P4 DSP engine over SPI.

## What You Can Build

The RP2350 controls the TBD-16's user interface and MIDI I/O. Your app has full access to:

- **50+ DSP plugins** on the ESP32-P4 via the SPI command API
- **OLED display** (SSD1309, 128×64, software SPI via PIO)
- **NeoPixel LED strip** (PIO-driven)
- **Rotary encoders and buttons** (via I2C polling of STM32F0)
- **USB MIDI host** (connect controllers like Arturia BeatStep Pro)
- **UART MIDI** (TRS MIDI in/out × 2)
- **8 MB PSRAM** and **SD card** (SDIO)

## Quick Start

### Prerequisites

- [PlatformIO](https://docs.platformio.org/en/latest/integration/ide/pioide.html) (recommended) or Arduino CLI
- TBD-16 hardware

### Build & Flash

```bash
# Clone this repo (or fork it for your own app)
git clone https://github.com/dadamachines/dada-tbd-app-template.git
cd dada-tbd-app-template

# Build
pio run -e pi2350

# Flash — enter BOOTSEL mode (hold right front button while connecting USB-C port #2)
pio run -e pi2350 -t upload
```

The `.uf2` output lands in `.pio/build/pi2350/firmware.uf2`. You can also drag-and-drop this file onto the RP2350 USB drive in BOOTSEL mode.

## Project Structure

```
src/
├── Midi.cpp/.h              USB + UART MIDI host and routing
├── MidiParser.cpp/.h        MIDI-to-SPI parameter mapping
├── MidiRunningStatusExpander.h  Running status → full messages
├── SpiAPI.cpp/.h            SPI command API to ESP32-P4
├── Ui.cpp/.h                OLED display, LEDs, encoders, buttons
├── DadaLogo.h               Boot logo bitmap
├── Fonts/                   OLED font data
└── src.ino                  Arduino entry point
examples/
└── main.cpp                 Example: load plugins, map MIDI, SPI API demos
platformio.ini               PlatformIO build configuration
pinmap.md                    Full GPIO pin assignments
architecture.png             System architecture diagram
Dockerfile                   Reproducible CI build environment
```

## Architecture

The TBD-16 has two processors connected via dual SPI buses:

| Bus | Speed | Purpose |
|-----|-------|---------|
| **SPI1** (real-time) | 30 MHz, ~1378 Hz | MIDI/CV/trigger data to DSP, audio levels + USB MIDI back |
| **SPI0** (command) | 30 MHz, on-demand | Plugin loading, parameter changes, preset management |

Both use 512-byte frames. See [SpiAPI.h](src/SpiAPI.h) for the full command reference and [pinmap.md](pinmap.md) for GPIO assignments.

## Key Classes

| Class | Purpose |
|-------|---------|
| `SpiAPI` | Send commands to the P4 DSP engine — load plugins, read/write parameters, manage presets, send files |
| `Midi` | Initialize USB host + UART MIDI, route messages, manage USB power and mux GPIOs |
| `MidiParser` | Map parsed MIDI events (notes, CCs) to SPI real-time control data for the DSP |
| `Ui` | Drive the OLED display, NeoPixel strip, and poll encoders/buttons via I2C |

## Examples

The [examples/main.cpp](examples/main.cpp) file demonstrates:

- **Real-time CV/Trigger API** — send control data directly to plugins without the MIDI parser
- **Load and map plugins** — load a DrumRack plugin and map note-ons to triggers
- **SPI API tests** — exercise the command API (plugin state, parameters, presets)

To explore, look at `Ui::Init()` and `Ui::Update()` for the call sites.

## Communication Interfaces

- **SPI1** — Real-time audio control at 44100/32 Hz (~1378 Hz). RP2350 sends MIDI/CV data, P4 returns USB MIDI device data and audio level indicators.
- **SPI0** — On-demand command API for plugin management, parameter changes, preset storage, file transfers.
- **OLED** — Software SPI via PIO (SSD1309, 128×64).
- **NeoPixel** — PIO-driven LED strip.
- **USB Host** — USB MIDI controllers. Requires GPIO-controlled power and USB mux enable (see `Midi::Init()`).
- **UART** — Two MIDI I/O ports (TRS connectors).
- **I2C** — Polls STM32F0 for encoder positions and button states.

## Creating Your Own App

1. **Fork this repo** on GitHub
2. Modify `src/` — add your instrument logic, UI pages, MIDI mappings
3. Build with `pio run -e pi2350`
4. Flash via BOOTSEL or copy `.uf2` to the Pico SD card (multi-app mode)

You can also use any RP2350 toolchain (Arduino IDE, Pico SDK, Rust) — the SPI protocol is documented in [SpiAPI.h](src/SpiAPI.h).

### Publishing Your App

To share your app with the TBD community:

1. Create a GitHub Release with your `.uf2` attached
2. Open a PR to [dada-tbd-firmware](https://github.com/dadamachines/dada-tbd-firmware) adding a manifest under `apps/your-app/manifest.json`
3. CI validates your manifest and binary — dadamachines reviews and merges
4. Your app appears in the App Manager catalog

This is optional — sideloading works without any registration.

## Dependencies

Managed automatically by PlatformIO:

| Library | Source |
|---------|--------|
| Adafruit GFX | `adafruit/Adafruit GFX Library@^1.12.1` |
| Adafruit NeoPixel | `adafruit/Adafruit NeoPixel@^1.12.4` |
| ArduinoJson | `ArduinoJson@^7.4.2` |
| TinyUSB (USB host fork) | `ctag-fh-kiel/Adafruit_TinyUSB_Arduino#fix/usb_host_arturia_bspro` |
| USB MIDI Host | `rppicomidi/usb_midi_host#1.1.4` |
| DaDa OLED | `ctag-fh-kiel/DaDa_OLED#v1.0.0` |
| DaDa SPI | `ctag-fh-kiel/DaDa_SPI#v1.0.5` |

## Building with Arduino CLI

If you prefer Arduino CLI over PlatformIO, you'll need to manually install the dependencies listed above, then:

```bash
cd src
arduino-cli compile \
  --fqbn rp2040:rp2040:generic_rp2350:variantchip=RP2530B,usbstack=tinyusb_host \
  --build-path ./build
```

Flash with `cp build/src.uf2 /Volumes/RP2350`.

## Debugging

Uncomment the debug lines in `platformio.ini` to use a CMSIS-DAP debugger (e.g. Pico Debug Probe):

```ini
debug_tool = cmsis-dap
upload_protocol = cmsis-dap
```

## Branches

This repo contains several branches, each with a different firmware variant:

### `main` — App Template (Arduino)

The default branch. A full Arduino-framework template for building custom TBD-16 apps with OLED UI, MIDI, NeoPixels, and the SPI command API to the ESP32-P4 DSP engine. See the rest of this README for details.

### `feature/tusb-msc-flash` — USB Mass Storage (Flash)

Flash-targeted USB Mass Storage firmware with OLED status display. Exposes the TBD-16's SD card as a USB drive for file management.

- **Framework:** Arduino (earlephilhower core) + Adafruit TinyUSB + SdFat
- **Binary type:** Standard flash (UF2 addresses `0x10000000`+)
- **OLED:** PIO SPI via DaDa_OLED / Adafruit GFX — shows mount/ready/eject status
- **Flash via:** Picoboot WebUSB (App Manager) or drag-and-drop in BOOTSEL mode
- **Build:** `pio run -e pi2350`
- **CDN:** Published as `tusb-msc-pico` (flash target)

### `feature/tusb-msc-oled` — USB Mass Storage (RAM / SD Bootloader)

RAM-targeted USB Mass Storage firmware with OLED status display. Same functionality as `feature/tusb-msc-flash` but designed to be loaded by the SD card bootloader at runtime.

- **Framework:** Pico SDK (raw C) + TinyUSB + no-OS-FatFS (SDIO PIO)
- **Binary type:** `no_flash` (UF2 addresses `0x20000000`+, requires `-DPICO_NO_FLASH=1`)
- **OLED:** Hardware SPI1 via lcdspi library — shows mount/ready/eject status
- **Flash via:** Copy `tusb_msc.uf2` to SD card `tbd-apps/` folder (multi-app bootloader)
- **Build:** `pio run -e pi2350`
- **CDN:** Published as `tusb-msc-pico` (RAM target)

### `feature/flash-nuke` — Flash Nuke

Erases the entire RP2350 flash chip. Compatible with Picoboot WebUSB flashing.

- **Framework:** Pico SDK (raw C)
- **Binary type:** `copy_to_ram` (UF2 addresses `0x10000000`+, CRT0 copies to RAM before executing)
- **Flash via:** Picoboot WebUSB (App Manager) or drag-and-drop in BOOTSEL mode
- **Build:** `pio run -e pi2350`
- **CDN:** Published as `flash-nuke`

## Community & Support

- [dadamachines Forum](https://forum.dadamachines.com) — Ask questions, share apps, connect with other TBD developers
- [GitHub Issues](https://github.com/dadamachines/dada-tbd-app-template/issues) — Bug reports and feature requests
- [TBD-16 Documentation](https://dadamachines.github.io/ctag-tbd/) — Full platform docs, plugin reference, flashing guides

## Acknowledgements

The TBD platform and SPI API were created by [Robert Manzke](https://github.com/ctag-fh-kiel/ctag-tbd) at the [Creative Technologies AG](https://www.creative-technologies.de/), Kiel University of Applied Sciences.

The TBD-16 adaptation is led by [dadamachines](https://dadamachines.com).

## License

This repository is licensed under the [GNU Lesser General Public License (LGPL 3.0)](https://www.gnu.org/licenses/lgpl-3.0.txt).

**Your app code is yours** — the LGPL applies only to the template libraries (SpiAPI, Midi, Ui, etc.). The code you write that links against them can be any license you choose, including proprietary. If you modify the template libraries themselves and distribute the result, share those modifications under LGPL 3.0.

See [LICENSE](LICENSE) for full details including trademark and commercial use terms.