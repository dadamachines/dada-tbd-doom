// TBD-16 Board Definition for RP2040-Doom
// DaDa Machines TBD-16: RP2350 + ESP32-P4 + STM32 UI Board
//
// This board header adapts the rp2040-doom project to the TBD-16 hardware.
// Key differences from jthumby.h:
//   - RP2350 (Cortex-M33) instead of RP2040 (Cortex-M0+)
//   - SSD1309 128x64 OLED instead of SSD1306 72x40
//   - Buttons via I2C (STM32 UIboard) instead of direct GPIO
//   - 8MB PSRAM available
//   - SD card available for WAD storage
//
// -----------------------------------------------------
// NOTE: THIS HEADER IS ALSO INCLUDED BY ASSEMBLER SO
//       SHOULD ONLY CONSIST OF PREPROCESSOR DIRECTIVES
// -----------------------------------------------------

#ifndef _BOARDS_JTBD16_H
#define _BOARDS_JTBD16_H

// For board detection
#define JTBD16 1

// pico_cmake_set PICO_PLATFORM=rp2350

// --- BOARD SPECIFIC ---
// OLED: SSD1309, 128x64, PIO SPI
#define J_OLED_CS 13
#define J_OLED_RESET 16
#define J_OLED_DC 12
#define J_OLED_MOSI 15
#define J_OLED_SCLK 14
// Frame period in us (60 Hz = 16667us per game frame)
#define J_OLED_FRAME_PERIOD 16667

// ======== Display rendering configuration ========
// SSD1309 128x64 greyscale: 3-pass bit-plane rendering with
// contrast-weighted sub-frames. ~7 perceived grey levels,
// same technique as the original rp2040-doom SSD1306 driver.
// No parking needed — contrast command is 2 bytes (~2µs),
// much shorter than one COM scan line (~150µs).
//
// Shadow-lift LUT: set to 1 to apply a pow(0.625) remap that
// opens up dark DOOM areas. Set to 0 for raw palette values.
#ifndef JTBD16_SHADOW_LIFT
#define JTBD16_SHADOW_LIFT 1
#endif

// Display resolution
#define J_DISPLAY_WIDTH 128
#define J_DISPLAY_HEIGHT 64

// --- STM32 UI Board (I2C) ---
#define J_I2C_UI_SDA 38
#define J_I2C_UI_SCL 39
#define J_I2C_UI_ADDR 0x42
#define J_I2C_UI_FREQ 400000

// --- MCL Button bit indices (REV_C) ---
#define J_MCL_LEFT    0
#define J_MCL_DOWN    1
#define J_MCL_RIGHT   2
#define J_MCL_UP      3
#define J_MCL_A       4   // FUNC5
#define J_MCL_B       5   // FUNC6
#define J_MCL_X       6   // MASTER
#define J_MCL_Y       7   // SOUND
#define J_MCL_PLAY    8   // Play/Pause
#define J_MCL_REC     9   // Record
#define J_MCL_S1      10  // Shift 1
#define J_MCL_S2      11  // Shift 2

// --- Favorite button (direct GPIO) ---
#define J_FAVORITE_PIN 25

// --- NeoPixel ---
#define J_NEOPIXEL_PIN 26
#define J_NEOPIXEL_COUNT 21

// --- Green LED ---
#define J_GREEN_LED_PIN 24

// --- SD Card (SDIO) ---
#define J_SD_CLK 2
#define J_SD_CMD 3
#define J_SD_DAT0 4
#define J_SD_DAT1 5
#define J_SD_DAT2 6
#define J_SD_DAT3 7
#define J_SD_DETECT 8

// --- PSRAM ---
#define J_PSRAM_CS 19
#define J_PSRAM_SIZE (8 * 1024 * 1024)

// --- UART (Debug via Pico Debug Probe) ---
// UART1 TX on GPIO20 is connected to the debug probe
#define J_DEBUG_UART uart1
#define J_DEBUG_UART_TX_PIN 20
#define J_DEBUG_UART_BAUD 115200

#ifndef PICO_DEFAULT_UART
#define PICO_DEFAULT_UART 1
#endif

#ifndef PICO_DEFAULT_UART_TX_PIN
#define PICO_DEFAULT_UART_TX_PIN 20
#endif

#ifndef PICO_DEFAULT_UART_RX_PIN
#define PICO_DEFAULT_UART_RX_PIN 45
#endif

// --- LED ---
#ifndef PICO_DEFAULT_LED_PIN
#define PICO_DEFAULT_LED_PIN 24
#endif

// --- I2C ---
// Default I2C is the UI board I2C
#ifndef PICO_DEFAULT_I2C
#define PICO_DEFAULT_I2C 1
#endif
#ifndef PICO_DEFAULT_I2C_SDA_PIN
#define PICO_DEFAULT_I2C_SDA_PIN 38
#endif
#ifndef PICO_DEFAULT_I2C_SCL_PIN
#define PICO_DEFAULT_I2C_SCL_PIN 39
#endif

// --- SPI ---
// Default SPI is the OLED PIO SPI (directly driven, not using HW SPI peripheral)
// HW SPI0 is used for P4 communication (not needed for Doom)
#ifndef PICO_DEFAULT_SPI
#define PICO_DEFAULT_SPI 0
#endif
#ifndef PICO_DEFAULT_SPI_SCK_PIN
#define PICO_DEFAULT_SPI_SCK_PIN 14
#endif
#ifndef PICO_DEFAULT_SPI_TX_PIN
#define PICO_DEFAULT_SPI_TX_PIN 15
#endif
#ifndef PICO_DEFAULT_SPI_RX_PIN
#define PICO_DEFAULT_SPI_RX_PIN 12
#endif
#ifndef PICO_DEFAULT_SPI_CSN_PIN
#define PICO_DEFAULT_SPI_CSN_PIN 13
#endif

// --- FLASH ---
#define PICO_BOOT_STAGE2_CHOOSE_W25Q080 1

#ifndef PICO_FLASH_SPI_CLKDIV
#define PICO_FLASH_SPI_CLKDIV 2
#endif

#ifndef PICO_FLASH_SIZE_BYTES
#define PICO_FLASH_SIZE_BYTES (16 * 1024 * 1024)
#endif

#endif // _BOARDS_JTBD16_H
