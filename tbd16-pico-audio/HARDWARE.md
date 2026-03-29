# TBD-16 Hardware Architecture — RP2350 Perspective

Complete pin map and peripheral reference for the DaDa Machines TBD-16 board, as seen from the RP2350B.

Use this when porting any RP2040/RP2350 project to TBD-16 — it tells you which GPIOs are taken, what peripherals are available, and how each subsystem works.

---

## System Overview

The TBD-16 is a triple-processor platform:

| Processor | Variant | Role |
|---|---|---|
| **RP2350B** | 48-GPIO, Cortex-M33, 150 MHz | App runtime, display, input, audio transport |
| **ESP32-P4** | Main DSP | Audio codec, WiFi, synthesizer engine |
| **STM32F0** | I2C slave | Button/encoder/pot scanning (UI board) |

The RP2350 does **not** have direct access to the audio codec — all audio goes through SPI to the P4, which drives the DAC. This package handles that transport.

---

## Complete GPIO Map

| GPIO | Function | Peripheral | Notes |
|---|---|---|---|
| 0–1 | — | — | Unassigned |
| **2** | SD SCK | SPI0 | Boot-only, released after WAD load |
| **3** | SD MOSI | SPI0 | " |
| **4** | SD MISO | SPI0 | " |
| **5** | SD D1 | SDIO | " |
| **6** | SD D2 | SDIO | " |
| **7** | SD CS/D3 | SPI0 | " |
| **8** | SD Detect | GPIO input | |
| **9** | USB-A Power Sense | GPIO input | |
| **10** | USB-A Power Enable | GPIO output | |
| **11** | USB-A / USB-C Select | GPIO output | HIGH = USB-A |
| **12** | OLED DC | GPIO output | 0 = command, 1 = data |
| **13** | OLED CS | GPIO output | Active low |
| **14** | OLED SCK | PIO0 SM0 | Side-set pin, 10 MHz |
| **15** | OLED MOSI | PIO0 SM0 | OUT pin |
| **16** | OLED Reset | GPIO output | |
| **17** | SD Power Enable | GPIO output | |
| **18** | P4 Control RDY | GPIO input | Active HIGH = P4 ready for command |
| **19** | PSRAM CS1 | QMI XIP_CS1 | 8 MB APS6404, mapped at `0x11000000` |
| **20** | Debug TX / I2C0 SDA | UART1 / I2C0 | Dual-use: debug output or expansion |
| **21** | I2C0 SCL | I2C0 | Expansion header |
| **22** | P4 Audio RDY | GPIO input | Active HIGH = P4 ready for SPI frame |
| **23** | — | — | Unassigned |
| **24** | Green LED | GPIO output | |
| **25** | Favorite Button | GPIO input | |
| **26** | NeoPixel | GPIO output | Up to 21 LEDs via UI board |
| **27** | Codec Word Clock | PWM slice 5 | 44100 Hz from P4 I2S WS |
| **28** | SPI1 MISO | SPI1 | P4 audio link — response (ignored) |
| **29** | SPI1 CSn | SPI1 | P4 audio link — hardware CS |
| **30** | SPI1 SCK | SPI1 | P4 audio link — 25 MHz |
| **31** | SPI1 MOSI | SPI1 | P4 audio link — audio data |
| **32** | SPI0 MISO | SPI0 | P4 control link (after SD release) |
| **33** | SPI0 CSn | SPI0 | " |
| **34** | SPI0 SCK | SPI0 | " |
| **35** | SPI0 MOSI | SPI0 | " |
| **36** | MIDI-OUT-1 | UART1 TX | |
| **37** | MIDI-IN-1 | UART1 RX | |
| **38** | I2C1 SDA | I2C1 | STM32 UI board |
| **39** | I2C1 SCL | I2C1 | STM32 UI board |
| **40** | STM32 Reset | GPIO output | |
| 41–43 | — | — | Not referenced in codebase |
| **44** | MIDI-OUT-2 | UART0 TX | |
| **45** | MIDI-IN-2 | UART0 RX | |
| 46–47 | — | — | Not referenced in codebase |

---

## Peripheral Usage

| Resource | Assignment | Details |
|---|---|---|
| **SPI0** | SD card → P4 control | Time-shared: GPIO 2–7 at boot, then GPIO 32–35 |
| **SPI1** | P4 audio link | 25 MHz Mode 0, 512-byte DMA frames |
| **PIO0 SM0** | OLED SPI | 10 MHz, 1 DMA channel (claimed dynamically) |
| **I2C0** | Expansion header | GPIO 20/21, optional |
| **I2C1** | STM32 UI board | 400 kHz, slave address `0x42` |
| **UART0** | MIDI port 2 | GPIO 44/45 |
| **UART1** | Debug TX + MIDI port 1 | GPIO 20 (debug) or 36/37 (MIDI) |
| **PWM slice 5** | Word-clock counter | GPIO 27, falling-edge counter |
| **QMI CS1** | PSRAM | 8 MB, mapped `0x11000000` |
| **TIMER0** | System timestamps | SDK default, `TIMERAWL` |
| **TIMER1** | Audio ISR | Alarm0 at 5000 Hz, IRQ 4 |
| **DMA 4** | SPI1 TX (audio) | DREQ 26 |
| **DMA 5** | SPI1 RX (audio) | DREQ 27 |

---

## Audio Transport (SPI1 → P4 → Codec)

This is what the `tbd16-pico-audio` driver handles.

```
RP2350 Ring Buffer → TIMER1 ISR → SPI1 DMA → ESP32-P4 → TLV320AIC3254 → Speaker
      (2048 pairs)    (5000 Hz)    (25 MHz)   (resample)    (44100 Hz)
```

| Signal | RP2350 GPIO | P4 GPIO | Direction |
|---|---|---|---|
| MISO | 28 | 29 | P4 → RP |
| CS | 29 | 28 | RP → P4 |
| SCLK | 30 | 30 | RP → P4 |
| MOSI | 31 | 31 | RP → P4 |
| Handshake | 22 (in) | 51 (out) | P4 → RP |
| Word Clock | 27 (in) | I2S WS | P4 → RP |

The P4 runs the **PicoAudioBridge** plugin which:
- Validates frames (0xCAFE + CRC)
- Anti-alias filters (20 kHz biquad LPF)
- Resamples your rate → 44100 Hz (cubic Hermite)
- Applies ×8 gain + hard clamp

---

## P4 Control Link (SPI0 → P4)

Used once at boot to activate the PicoAudioBridge plugin.

| Signal | RP2350 GPIO | P4 GPIO |
|---|---|---|
| MISO | 32 | 22 |
| CS | 33 | 20 |
| SCLK | 34 | 21 |
| MOSI | 35 | 23 |
| RDY | 18 (in) | — |

**Important**: SPI0 is shared with the SD card (GPIO 2–7). Use the SD card first, then re-initialize SPI0 on GPIO 32–35 for the control link. The audio driver handles this automatically.

---

## OLED Display

| Parameter | Value |
|---|---|
| Controller | SSD1309 |
| Resolution | 128×64 pixels, 1-bit monochrome |
| Size | 2.4" diagonal |
| Interface | PIO0 SM0 SPI, 10 MHz |
| DMA | 1 channel (dynamically claimed) |
| Framebuffer | 1024 bytes (128 cols × 8 pages) |
| Orientation | Mounted 180° rotated — corrected in software |

| Pin | GPIO | Function |
|---|---|---|
| SCK | 14 | PIO side-set |
| MOSI | 15 | PIO OUT |
| CS | 13 | GPIO, active low |
| DC | 12 | GPIO: 0=command, 1=data |
| RESET | 16 | GPIO |

GPIO 14/15 are the same physical pins as hardware SPI1 RX/CS, but driven by PIO — SPI1 stays free for audio.

---

## SD Card

| Parameter | Value |
|---|---|
| Interface | SPI0, GPIO 2–7 |
| Speed | 300 kHz init → 25 MHz normal |
| Library | Petit FatFS (read-only, no malloc) |
| Power control | GPIO 17 (enable), GPIO 8 (detect) |

Used at boot to load data into PSRAM. After boot, SPI0 is released and re-used for the P4 control link.

Both the P4 and RP2350 can access the SD card — the P4 uses SDMMC (GPIO 39–44), the RP2350 uses SPI0 (GPIO 2–7). They access different pins on the same physical card.

---

## PSRAM

| Parameter | Value |
|---|---|
| Chip | APS6404L-3SQR-SN |
| Size | 8 MB |
| Interface | QMI CS1 (XIP), GPIO 19 |
| Mapped address | `0x11000000` |
| Max clock | 109 MHz |
| Mode | QPI (quad SPI) |

**Gotcha**: On RP2350, including Pico SDK headers (`hardware/timer.h`, `stdbool.h`) before `stdio.h` can hang PSRAM initialization. Always `#include <stdio.h>` first in files that run during early boot. The audio driver uses direct register access to avoid this.

---

## Buttons & Input (I2C1 → STM32)

The STM32F0 on the UI board scans all physical controls and provides them over I2C1.

| Parameter | Value |
|---|---|
| Interface | I2C1, 400 kHz |
| Address | `0x42` |
| SDA / SCL | GPIO 38 / 39 |
| STM32 Reset | GPIO 40 |

**Data returned** (`ui_data_t`):
- 8× pot ADC values
- 4× pot positions
- 16× digital buttons
- 6× function buttons
- 12× MCL buttons (D-pad, A/B/X/Y, Play, Rec, S1, S2)
- 3-axis accelerometer
- Systick timestamp

**MCL button bit mapping**: 0=LEFT, 1=DOWN, 2=RIGHT, 3=UP, 4=A, 5=B, 6=X(MASTER), 7=Y(SOUND), 8=PLAY, 9=REC, 10=S1, 11=S2

---

## MIDI

Two independent MIDI ports:

| Port | TX GPIO | RX GPIO | UART |
|---|---|---|---|
| MIDI-1 | 36 | 37 | UART1 |
| MIDI-2 | 44 | 45 | UART0 |

Standard MIDI baud rate (31250).

---

## Other GPIO

| GPIO | Function | Direction | Notes |
|---|---|---|---|
| 24 | Green LED | Output | |
| 25 | Favorite button | Input | |
| 26 | NeoPixel data | Output | Up to 21 LEDs via UI board |
| 9 | USB-A power sense | Input | |
| 10 | USB-A power enable | Output | |
| 11 | USB-A/C select | Output | HIGH = USB-A host |

---

## Resource Conflicts to Watch

When porting a project, watch for these conflicts:

1. **SPI0 is time-shared** — SD card (GPIO 2–7) at boot, then P4 control (GPIO 32–35). Don't use SPI0 for other things.

2. **DMA channels 4 and 5** are hard-assigned to audio. Don't claim them for other purposes.

3. **TIMER1** is used by the audio ISR. Use TIMER0 for your own timing.

4. **PIO0 SM0** is used by the OLED. PIO0 SM1–3 and all of PIO1 are free.

5. **GPIO 14/15** look like SPI1 pins but are actually PIO (OLED). SPI1 uses GPIO 28–31.

6. **PWM slice 5** is the word-clock counter — don't use GPIO 27 for PWM output.

7. **GPIO 19** is PSRAM — never reassign it.
