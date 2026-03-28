# Complete Hardware SPI Inventory - dada-tbd-doom

## Executive Summary
Repository contains **3 independent hardware SPI interfaces**:
1. **OLED Display** (SPI1) - 8 MHz, using jtbd16.h OLED pins
2. **SD Card** (SPI0) - Variable speed (300 kHz init, 25 MHz runtime), using diskio.c SD pins
3. **Control SPI** (SPI0 Secondary) - 30 MHz for P4 communication (not currently used in Doom)

**CRITICAL ISSUE**: OLED uses SPI1 (GPIO 14/15), SD uses SPI0 (GPIO 2/3/4), but the comment in jtbd16.h mentions "OLED PIO SPI" which appears to be **planned future work, not current implementation**. Code actually uses **hardware SPI calls** for OLED.

---

## 1. OLED DISPLAY (SSD1309 128x64)

### Current Status: Hardware SPI (Not PIO)
**File**: [lib/lcdspi/ssd1309.c](lib/lcdspi/ssd1309.c)
**Header**: [lib/lcdspi/ssd1309.h](lib/lcdspi/ssd1309.h)

### SPI Configuration
- **Instance**: SPI1 (`spi1`)
- **Speed**: 8 MHz
- **Format**: 8-bit, CPOL=0, CPHA=0, MSB_FIRST

### GPIO Pins (from jtbd16.h)
| Signal | GPIO | Function |
|--------|------|----------|
| OLED_MOSI | 15 | GPIO_FUNC_SPI (spi1 TX) |
| OLED_SCLK | 14 | GPIO_FUNC_SPI (spi1 SCK) |
| OLED_DC | 12 | GPIO_OUT (command/data select) |
| OLED_CS | 13 | GPIO_OUT (chip select) |
| OLED_RESET | 16 | GPIO_OUT (reset) |

### Code References
- **Init**: [i_video.c:658-661](lib/rp2040-doom/src/pico/i_video.c#L658-L661) - `power_on_logo()` function
- **Display**: [i_video.c:744-747](lib/rp2040-doom/src/pico/i_video.c#L744-L747) - `display_driver_init()` function
- **SPI Calls**:
  - Line 658-661: GPIO setup + spi_init + spi_set_format
  - Line 676: `spi_write_blocking(TBD_SPI, cmd_init, sizeof(cmd_init))`
  - Line 690: `spi_write_blocking(TBD_SPI, zeros, 128)` (clearing screen)
  - Monochrome rendering: [ssd1309.c:180](lib/lcdspi/ssd1309.c#L180) `spi_write_blocking(SSD1309_SPI_INST, &display_buffer[page * SSD1309_WIDTH], SSD1309_WIDTH)`

### Macro Definitions
```c
#define SSD1309_SPI_INST    spi1
#define SSD1309_SPI_SPEED   8000000
```

### Display Functions
- `ssd1309_spi_init()` - [Line 58](lib/lcdspi/ssd1309.c#L58)
- `ssd1309_command()` - [Line 80](lib/lcdspi/ssd1309.c#L80) - DC=0, then spi_write_blocking
- `ssd1309_command_list()` - [Line 88](lib/lcdspi/ssd1309.c#L88)
- `ssd1309_data()` - [Line 97](lib/lcdspi/ssd1309.c#L97) - DC=1, then spi_write_blocking
- `ssd1309_display()` - [Line 164](lib/lcdspi/ssd1309.c#L164) - Writes 128x64/8 = 1024 bytes in 8 pages

### Initialization Flow
1. **power_on_logo()**: [i_video.c:651](lib/rp2040-doom/src/pico/i_video.c#L651)
   - GPIO init for CS, RESET, DC
   - GPIO function setup for SCLK (14) and MOSI (15)
   - `spi_init(TBD_SPI, 8000000)`
   - SSD1309 reset sequence
   - Send init commands (31 bytes)
   - Clear screen (8 × 128 bytes)

2. **display_driver_init()**: [i_video.c:732](lib/rp2040-doom/src/pico/i_video.c#L732)
   - Repeat GPIO setup
   - Send init commands again
   - Ready for game rendering

---

## 2. SD CARD (SDIO or SPI Mode)

### Current Status: Hardware SPI (SPI0)
**File**: [lib/petit_fatfs/diskio.c](lib/petit_fatfs/diskio.c)
**Code**: Lines 43, 98, 109, 305-308, 374

### SPI Configuration
- **Instance**: SPI0 (`spi0`)
- **Speed (Init)**: 300 kHz
- **Speed (Runtime)**: 25 MHz
- **Pins**: SD_SPICH = spi0

### GPIO Pins (from jtbd16.h)
| Signal | GPIO | Function | diskio.c Define |
|--------|------|----------|-----------------|
| SD_DAT0 | 4 | GPIO/SPI0 MISO | SD_SPI_MISO |
| SD_CMD | 3 | GPIO/SPI0 MOSI | SD_SPI_MOSI |
| SD_CLK | 2 | GPIO/SPI0 SCK | SD_SPI_SCK |
| SD_DAT3 | 7 | GPIO_OUT (CS) | SD_SPI_CS |
| SD Power Enable | 17 | GPIO_OUT | SD_RESET_PIN |
| SD Detect | 8 | (detection input) | J_SD_DETECT |

**Alternative pins in jtbd16.h**:
- SD_CLK: GPIO 2
- SD_CMD: GPIO 3
- SD_DAT0: GPIO 4
- SD_DAT1: GPIO 5
- SD_DAT2: GPIO 6
- SD_DAT3: GPIO 7
- SD_DETECT: GPIO 8
- SD_POWER_EN: GPIO 17

### Initialization Sequence
[diskio.c:305-315](lib/petit_fatfs/diskio.c#L305-L315):
```c
spi_init(SD_SPICH, SD_SPI_BAUDRATE_INIT);  // 300 kHz
gpio_set_function(SD_SPI_MISO, GPIO_FUNC_SPI);  // GPIO 4
gpio_set_function(SD_SPI_MOSI, GPIO_FUNC_SPI);  // GPIO 3
gpio_set_function(SD_SPI_SCK, GPIO_FUNC_SPI);   // GPIO 2
gpio_set_pulls(SD_SPI_MISO, true, false);  // pull-up on MISO
gpio_init(SD_SPI_CS);  // GPIO 7
gpio_set_dir(SD_SPI_CS, GPIO_OUT);

// Power cycle
gpio_set_dir(SD_RESET_PIN, GPIO_OUT);  // GPIO 17
gpio_put(SD_RESET_PIN, 1);
sleep_us(100000);
gpio_put(SD_RESET_PIN, 0);
sleep_us(100000);
```

### Speed Change
[diskio.c:374](lib/petit_fatfs/diskio.c#L374):
```c
spi_init(SD_SPICH, SD_SPI_BAUDRATE);  // 25 MHz after initialization
```

### SPI Operations
- **Write**: [diskio.c:98](lib/petit_fatfs/diskio.c#L98) - `spi_write_blocking(SD_SPICH, buff, bc)`
- **Read**: [diskio.c:109](lib/petit_fatfs/diskio.c#L109) - `spi_read_blocking(SD_SPICH, 0xFF, buff, bc)`

---

## 3. CONTROL SPI (P4/ESP32-P4 Communication)

### Current Status: Defined but NOT USED in Doom
**Files**: 
- [src/SpiAPI.h](src/SpiAPI.h#L46)
- [src/Midi.h](src/Midi.h#L29)

### Two Separate Control SPI Channels

#### A. Command API SPI (P4 MCU communication)
[src/SpiAPI.h:46](src/SpiAPI.h#L46):
```cpp
DaDa_SPI cmd_api_spi {spi0, 33, 35, 32, 34, 18, 30000000};
```
- **Instance**: SPI0
- **MOSI**: GPIO 33
- **MISO**: GPIO 35
- **CLK**: GPIO 32
- **CS**: GPIO 34
- **Extra Pin**: GPIO 18 (purpose unclear - possibly clock/ready signal)
- **Speed**: 30 MHz

**Protocol**: Binary command/response with handshake
- **Fingerprint**: 0xCA, 0xFE (2 bytes)
- **Request Type**: 1 byte
- **Length**: 4 bytes (uint32_t)
- **Payload**: variable cstring (JSON/binary)
- **Packet Size**: 2048 bytes

**Commands**: Supports 31+ message types ([RequestType_t](src/SpiAPI.h#L7-L35)):
- GetPlugins (0x01)
- SetActivePlugin (0x04) - with JSON plugin ID
- SetPluginParam (0x05)
- SetPluginParamCV (0x06), TRIG (0x07)
- SavePreset (0x0C), LoadPreset (0x0B)
- SaveFavorite (0x0E), LoadFavorite (0x0F)
- GetFirmwareInfo (0x19)

#### B. Real-Time MIDI SPI (P4 Real-Time Communication)
[src/Midi.h:29](src/Midi.h#L29):
```cpp
DaDa_SPI real_time_spi {spi1, 29, 31, 28, 30, 22, 30000000};
```
- **Instance**: SPI1
- **MOSI**: GPIO 29
- **MISO**: GPIO 31
- **CLK**: GPIO 28
- **CS**: GPIO 30
- **Extra Pin**: GPIO 22 (P4 alive status?)
- **Speed**: 30 MHz

**Purpose**: Real-time MIDI data streaming, CV/trigger updates
- **Buffer**: 240 CVs × 4 bytes + 60 triggers (N_CVS_TBD, N_TRIGS_TBD)
- **Protocol**: Sequence counter-based (100-199 range)
- **Features**:
  - Link tempo data (Ableton Link)
  - Real-time state mutations
  - P4 alive status monitoring

### DaDa_SPI Class
**Note**: DaDa_SPI header not found in workspace - likely in external Arduino library or build output.

**Constructor Pattern** (inferred from usage):
```cpp
DaDa_SPI(spi_inst_t *spi_instance, uint mosi_pin, uint miso_pin, uint clk_pin, uint cs_pin, uint aux_pin, uint32_t speed_hz)
```

**Methods**:
- `TransferBlockingDelayed(out_buf, in_buf, size)` - [SpiAPI.cpp:55](src/SpiAPI.cpp#L55)
- `WaitUntilP4IsReady()` - [SpiAPI.cpp:23](src/SpiAPI.cpp#L23)
- `GetP4Ready()` - [SpiAPI.cpp:26](src/SpiAPI.cpp#L26)

---

## 4. I2C KEYBOARD (STM32 UI Board)

### Configuration
**File**: [lib/i2ckbd/i2ckbd.h](lib/i2ckbd/i2ckbd.h)

| Property | Value |
|----------|-------|
| I2C Instance | i2c1 |
| SDA Pin | 38 |
| SCL Pin | 39 |
| Speed | 400 kHz |
| Address | 0x42 |

### Initialization
[lib/i2ckbd/i2ckbd.c:235-241](lib/i2ckbd/i2ckbd.c#L235-L241):
```c
gpio_set_function(I2C_KBD_SCL, GPIO_FUNC_I2C);  // GPIO 39
gpio_set_function(I2C_KBD_SDA, GPIO_FUNC_I2C);  // GPIO 38
i2c_init(I2C_KBD_MOD, I2C_KBD_SPEED);  // 400 kHz
gpio_pull_up(I2C_KBD_SCL);
gpio_pull_up(I2C_KBD_SDA);
```

### Button Mapping (REV_C)
[jtbd16.h:100-111](lib/rp2040-doom/boards/jtbd16.h#L100-L111):
- Bit 0: LEFT
- Bit 1: DOWN
- Bit 2: RIGHT
- Bit 3: UP
- Bit 4: A (FUNC5)
- Bit 5: B (FUNC6)
- Bit 6: X (MASTER)
- Bit 7: Y (SOUND)
- Bit 8: PLAY
- Bit 9: REC
- Bit 10: S1 (Shift 1)
- Bit 11: S2 (Shift 2)

---

## 5. PSRAM

**Configuration** (from jtbd16.h):
- CS: GPIO 19 (handled by boot)
- Size: 8 MB
- Mapped at: 0x11000000

---

## 6. PIO Usage

### Current PIO Programs
- **video_doom.pio**: [lib/rp2040-doom/src/pico/video_doom.pio](lib/rp2040-doom/src/pico/video_doom.pio)
  - Purpose: Video scanline rendering (not SPI)
  - Not used for JTBD16 (conditional: `#if !JTBD16`)

- **capsense.pio**: [lib/rp2040-doom/src/pico/capsense.pio](lib/rp2040-doom/src/pico/capsense.pio)
  - Purpose: Capacitive touch sensing
  - Status: Commented out (`#if PIO_CAPSENSE` - set to 0 by default)

- **SD Card PIO**: [.pico-extras/src/rp2_common/pico_sd_card/sd_card.c](pico-sd-card/sd_card.c#L46)
  - Instance: PIO1
  - Status: Not used (SD using hardware SPI0)
  - Note: Available as alternative implementation if needed

### No PIO Claimed by OLED Currently
The PINMAP.md shows "OLED PIO_SPI" allocation, but actual code uses hardware SPI1. This appears to be **planning documentation** for refactoring OLED to PIO.

---

## 7. Pin Collision Analysis

### **No Collisions** ✓
| Device | SPI Instance | Pins | Status |
|--------|--------------|------|--------|
| OLED | spi1 | 14 (SCK), 15 (MOSI) | Allocated |
| SD Card | spi0 | 2 (SCK), 3 (MOSI), 4 (MISO), 7 (CS) | Allocated |
| **Potential Conflict**: SpiAPI | spi0 | 32 (CLK), 33 (MOSI), 34 (CS), 35 (MISO) | **Not used in Doom** |
| **Potential Conflict**: Midi | spi1 | 28 (CLK), 29 (MOSI), 30 (CS), 31 (MISO) | **Not used in Doom** |
| I2C Keyboard | i2c1 | 38 (SDA), 39 (SCL) | Allocated |

**CRITICAL**: Control SPI (SpiAPI on spi0) and Midi SPI (on spi1) would **conflict** with OLED + SD if both were active simultaneously. Currently disabled in Doom build.

---

## 8. Summary Table: All SPI Resources

| Subsystem | HW Instance | GPIO Pins | Speed | Status | Conversion Target |
|-----------|--------------|-----------|-------|--------|-------------------|
| OLED Display | spi1 | 14 (SCK), 15 (MOSI), 13 (CS), 12 (DC) | 8 MHz | Active | **Convert to PIO** |
| SD Card | spi0 | 2, 3, 4, 7 | 300k→25M | Active | Keep as-is (WaD loading) |
| P4 Commands | spi0 | 32, 33, 34, 35 | 30 MHz | Disabled | Not in scope |
| P4 Real-Time | spi1 | 28, 29, 30, 31 | 30 MHz | Disabled | Not in scope |
| I2C Buttons | i2c1 | 38, 39 | 400 kHz | Active | Not SPI |

---

## Files to Modify for PIO Conversion
1. [lib/rp2040-doom/src/pico/i_video.c](lib/rp2040-doom/src/pico/i_video.c) - Replace `spi_write_blocking()` with PIO
2. [lib/lcdspi/ssd1309.c](lib/lcdspi/ssd1309.c) - Wrapper functions for SPI
3. [lib/lcdspi/ssd1309.h](lib/lcdspi/ssd1309.h) - Macro definitions
4. [lib/rp2040-doom/boards/jtbd16.h](lib/rp2040-doom/boards/jtbd16.h) - Pin defs (no change needed)
5. [lib/rp2040-doom/src/pico/CMakeLists.txt](lib/rp2040-doom/src/pico/CMakeLists.txt) - Add PIO header generation if creating new `.pio` file
