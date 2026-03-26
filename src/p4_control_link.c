// P4 Control Link — picosdk-native state API sender
// Link2 (spi0rp ↔ SPI3p4): low-speed control over spi0 (GPIO 32-35).
// Sends SetActivePlugin("PicoAudioBridge") to tell the P4 to accept audio.
//
// spi0 is shared with SD card (GPIO 2-7). This module initializes spi0 on
// different GPIO pins (32-35) AFTER the SD card has been deinitialized.
//
// CRITICAL WORKAROUND: On RP2350 with PSRAM, including stdbool.h in the wrong
// order causes PSRAM init to hang. To avoid this, we DON'T include any pico SDK
// headers that pull in pico/types.h. Instead we use direct register access.

#include <stdio.h>       // MUST be first and alone — avoids PSRAM hang
#include <string.h>
#include <stdlib.h>
#include <stdint.h>

// Direct register definitions for RP2350B (48 GPIO variant)
// NOTE: On RP2350B, ALL GPIOs (0-47) are in IO_BANK0 and PADS_BANK0.
// There is NO separate bank1 — 0x40030000 is IO_QSPI, not GPIO bank1!
#define SPI0_BASE           0x40080000  // RP2350 SPI0 (NOT 0x4003c000 which is RP2040!)
#define SIO_BASE            0xd0000000
#define PADS_BANK0_BASE     0x40038000  // Covers GPIO 0-47 on RP2350B
#define IO_BANK0_BASE       0x40028000  // Covers GPIO 0-47 on RP2350B
#define TIMER0_BASE         0x400b0000

// SPI registers
#define SPI_SSPCR0_OFFSET   0x00
#define SPI_SSPCR1_OFFSET   0x04
#define SPI_SSPDR_OFFSET    0x08
#define SPI_SSPSR_OFFSET    0x0c
#define SPI_SSPCPSR_OFFSET  0x10

// GPIO pad control (same offset formula for both banks)
#define PADS_GPIO_OFFSET(n) (4 + (n) * 4)

// GPIO function mux (same offset formula for both banks, adjusted per-bank)
#define IO_GPIO_CTRL_OFFSET(n) (4 + (n) * 8)
#define GPIO_FUNC_SPI       1
#define GPIO_FUNC_SIO       5

// SIO for direct GPIO (RP2350B has separate HI registers for GPIO 32-47)
#define SIO_GPIO_IN_OFFSET      0x004  // GPIO 0-31 input
#define SIO_GPIO_HI_IN_OFFSET   0x008  // GPIO 32-47 input (RP2350B)
#define SIO_GPIO_OUT_OFFSET     0x010  // GPIO 0-31 output
#define SIO_GPIO_HI_OUT_OFFSET  0x024  // GPIO 32-47 output (RP2350B)
#define SIO_GPIO_OE_OFFSET      0x030  // GPIO 0-31 output enable
#define SIO_GPIO_HI_OE_OFFSET   0x044  // GPIO 32-47 output enable (RP2350B)
#define SIO_GPIO_OE_SET_OFFSET  0x038  // GPIO 0-31 OE set
#define SIO_GPIO_HI_OE_SET_OFFSET 0x04c // GPIO 32-47 OE set (RP2350B)
#define SIO_GPIO_OE_CLR_OFFSET  0x040  // GPIO 0-31 OE clear
#define SIO_GPIO_HI_OE_CLR_OFFSET 0x054 // GPIO 32-47 OE clear (RP2350B)

// Timer for delays
#define TIMER_TIMERAWL_OFFSET 0x28

// Register access macros
#define REG32(base, off) (*(volatile uint32_t *)((base) + (off)))

// ── Pin assignments (Link2: spi0rp, matches tbd-pico-seq3 SpiAPI.h) ───
#define CTL_MISO_PIN  32   // SPI0 RX
#define CTL_CS_PIN    33   // SPI0 CSn (hardware-managed)
#define CTL_CLK_PIN   34   // SPI0 SCK
#define CTL_MOSI_PIN  35   // SPI0 TX
#define CTL_RDY_PIN   18   // P4 ready-for-command signal (active high)
#define CTL_WS_PIN    27   // Codec word-clock (44100 Hz I2S WS from P4)

#define CTL_SPI_FREQ  30000000  // 30 MHz

// ── Control link buffer size (matches P4 firmware) ─────────────────────
#define CTL_BUF_LEN   2048

// ── Protocol field offsets (matches SpiAPI.cpp layout) ─────────────────
#define FP_0           0   // 0xCA
#define FP_1           1   // 0xFE
#define REQ_TYPE       2   // request type byte
#define PARAM_0        3   // uint8 param (channel)
#define PARAM_1        4   // uint8 param (reserved)
#define PARAM_2        5   // int32 param (string length), 4 bytes
#define STRING_PARAM   9   // cstring payload starts here

// ── Request types (subset from SpiAPI.h) ───────────────────────────────
#define REQ_SET_ACTIVE_PLUGIN  0x04

// Static buffers to avoid heap/PSRAM issues after WAD load
static uint8_t ctl_out_buf[CTL_BUF_LEN];
static uint8_t ctl_in_buf[CTL_BUF_LEN];
static uint8_t *ctl_out = ctl_out_buf;
static uint8_t *ctl_in = ctl_in_buf;

static inline uint32_t get_time_us(void) {
    return REG32(TIMER0_BASE, TIMER_TIMERAWL_OFFSET);
}

static inline void delay_us(uint32_t us) {
    uint32_t start = get_time_us();
    while (get_time_us() - start < us) {}
}

static inline int gpio_read(uint32_t pin) {
    if (pin < 32) {
        return (REG32(SIO_BASE, SIO_GPIO_IN_OFFSET) >> pin) & 1;
    } else {
        // GPIO 32-47 use HI registers on RP2350B
        return (REG32(SIO_BASE, SIO_GPIO_HI_IN_OFFSET) >> (pin - 32)) & 1;
    }
}

static void gpio_set_input(uint32_t pin) {
    // All GPIO 0-47 use IO_BANK0 and PADS_BANK0 on RP2350B
    // Pad and IO ctrl offsets scale linearly with pin number
    REG32(PADS_BANK0_BASE, PADS_GPIO_OFFSET(pin)) = (1 << 6) | (1 << 2);  // IE + PDE
    REG32(IO_BANK0_BASE, IO_GPIO_CTRL_OFFSET(pin)) = GPIO_FUNC_SIO;
    // Clear output enable (make it an input)
    if (pin < 32) {
        REG32(SIO_BASE, SIO_GPIO_OE_CLR_OFFSET) = (1u << pin);
    } else {
        REG32(SIO_BASE, SIO_GPIO_HI_OE_CLR_OFFSET) = (1u << (pin - 32));
    }
}

static void gpio_set_function(uint32_t pin, uint32_t func) {
    // All GPIO 0-47 use IO_BANK0 and PADS_BANK0 on RP2350B
    // Configure pad: IE=1 (input enable), drive strength default
    REG32(PADS_BANK0_BASE, PADS_GPIO_OFFSET(pin)) = (1 << 6);  // IE=1
    // Set function mux
    REG32(IO_BANK0_BASE, IO_GPIO_CTRL_OFFSET(pin)) = func;
}

// Resets controller (to bring SPI0 out of reset after spi_deinit)
#define RESETS_BASE             0x40020000
#define RESETS_RESET_OFFSET     0x00
#define RESETS_RESET_DONE_OFFSET 0x08
#define RESETS_SPI0_BIT         (1u << 18)
// RP2350 atomic register access: base+0x3000 = atomic clear
#define RESETS_ATOMIC_CLR       0x3000

static void spi0_unreset(void) {
    // Clear SPI0 reset bit (take out of reset)
    REG32(RESETS_BASE + RESETS_ATOMIC_CLR, RESETS_RESET_OFFSET) = RESETS_SPI0_BIT;
    // Wait for SPI0 to come out of reset
    while (!(REG32(RESETS_BASE, RESETS_RESET_DONE_OFFSET) & RESETS_SPI0_BIT)) {}
}

void p4_control_link_init(void) {
    printf("[P4-CTL] init...\n");

    // SPI0 was put into reset by spi_deinit() after SD card use.
    // Must bring it back out of reset before touching SPI0 registers.
    spi0_unreset();

    // Configure SPI0 for 30 MHz, Mode 3 (CPOL=1, CPHA=1), 8-bit
    uint32_t clkdiv = 150000000 / CTL_SPI_FREQ;
    if (clkdiv < 2) clkdiv = 2;
    
    REG32(SPI0_BASE, SPI_SSPCPSR_OFFSET) = clkdiv;
    REG32(SPI0_BASE, SPI_SSPCR0_OFFSET) = (1 << 7) | (1 << 6) | 0x07;
    REG32(SPI0_BASE, SPI_SSPCR1_OFFSET) = (1 << 1);

    // Configure GPIO for SPI function (bank 1 on RP2350B)
    gpio_set_function(CTL_MISO_PIN, GPIO_FUNC_SPI);
    gpio_set_function(CTL_CS_PIN,   GPIO_FUNC_SPI);
    gpio_set_function(CTL_CLK_PIN,  GPIO_FUNC_SPI);
    gpio_set_function(CTL_MOSI_PIN, GPIO_FUNC_SPI);

    // RDY input from P4 (active high)
    gpio_set_input(CTL_RDY_PIN);

    // Set fingerprint in output buffer
    memset(ctl_out, 0, CTL_BUF_LEN);
    ctl_out[FP_0] = 0xCA;
    ctl_out[FP_1] = 0xFE;

    printf("[P4-CTL] Initialized (spi0 Mode3 @ %uMHz, SCK=%d MOSI=%d MISO=%d CS=%d RDY=%d)\n",
           CTL_SPI_FREQ / 1000000, CTL_CLK_PIN, CTL_MOSI_PIN, CTL_MISO_PIN, CTL_CS_PIN, CTL_RDY_PIN);
}

int p4_control_link_wait_alive(uint32_t min_pulses, uint32_t timeout_ms) {
    // Count falling edges on WS pin (GPIO 27) to detect P4 codec activity
    gpio_set_input(CTL_WS_PIN);  // Uses our direct register function

    uint32_t pulses = 0;
    uint32_t deadline_us = get_time_us() + (timeout_ms * 1000);

    printf("[P4-CTL] Waiting for P4 codec word-clock on GPIO%d (%lu pulses, %lu ms timeout)...\n",
           CTL_WS_PIN, (unsigned long)min_pulses, (unsigned long)timeout_ms);

    int last_state = gpio_read(CTL_WS_PIN);
    while (pulses < min_pulses) {
        if ((int32_t)(get_time_us() - deadline_us) >= 0) {
            printf("[P4-CTL] Timeout — P4 codec not detected (%lu/%lu pulses)\n",
                   (unsigned long)pulses, (unsigned long)min_pulses);
            return 0;
        }
        int cur = gpio_read(CTL_WS_PIN);
        if (last_state && !cur) {  // falling edge
            pulses++;
        }
        last_state = cur;
    }

    printf("[P4-CTL] P4 alive! (%lu word-clock pulses detected)\n", (unsigned long)pulses);
    return 1;
}

// ── Direct SPI transfer (no SDK) ──────────────────────────────────────
static void spi_write_read(const uint8_t *tx, uint8_t *rx, size_t len) {
    for (size_t i = 0; i < len; i++) {
        // Wait for TX FIFO not full
        while (!(REG32(SPI0_BASE, SPI_SSPSR_OFFSET) & (1 << 1))) {}  // TNF
        REG32(SPI0_BASE, SPI_SSPDR_OFFSET) = tx[i];
        
        // Wait for RX FIFO not empty
        while (!(REG32(SPI0_BASE, SPI_SSPSR_OFFSET) & (1 << 2))) {}  // RNE
        rx[i] = REG32(SPI0_BASE, SPI_SSPDR_OFFSET) & 0xFF;
    }
}

// ── Blocking SPI transfer with P4 ready check ─────────────────────────
static int ctl_transfer(void) {
    // Wait for P4 ready (timeout 100ms)
    uint32_t deadline_us = get_time_us() + (100 * 1000);
    while (!gpio_read(CTL_RDY_PIN)) {
        if ((int32_t)(get_time_us() - deadline_us) >= 0) {
            printf("[P4-CTL] P4 not ready (RDY timeout)\n");
            return 0;
        }
    }

    spi_write_read(ctl_out, ctl_in, CTL_BUF_LEN);
    delay_us(15);  // post-transfer delay (matches DaDa_SPI)

    // Verify fingerprint in response
    if (ctl_in[FP_0] != 0xCA || ctl_in[FP_1] != 0xFE) {
        printf("[P4-CTL] Bad response fingerprint: 0x%02X 0x%02X\n",
               ctl_in[FP_0], ctl_in[FP_1]);
        return 0;
    }

    return 1;
}

int p4_control_link_set_active_plugin(uint8_t channel, const char *plugin_name) {
    uint32_t name_len = strlen(plugin_name);

    // Pack command
    memset(ctl_out + REQ_TYPE, 0, CTL_BUF_LEN - REQ_TYPE);
    ctl_out[FP_0] = 0xCA;
    ctl_out[FP_1] = 0xFE;
    ctl_out[REQ_TYPE] = REQ_SET_ACTIVE_PLUGIN;
    ctl_out[PARAM_0] = channel;
    ctl_out[PARAM_1] = 0;  // reserved

    // String length as int32 at offset 5
    int32_t len32 = (int32_t)name_len;
    memcpy(&ctl_out[PARAM_2], &len32, sizeof(len32));

    // Plugin name (null-terminated)
    if (name_len + 1 > CTL_BUF_LEN - STRING_PARAM) {
        name_len = CTL_BUF_LEN - STRING_PARAM - 1;
    }
    memcpy(&ctl_out[STRING_PARAM], plugin_name, name_len);
    ctl_out[STRING_PARAM + name_len] = '\0';

    printf("[P4-CTL] SetActivePlugin(ch=%d, \"%s\")\n", channel, plugin_name);

    if (!ctl_transfer()) {
        printf("[P4-CTL] SetActivePlugin transfer failed\n");
        return 0;
    }

    // P4 processes command after the SPI transfer completes,
    // so it can't echo the request type in the same response.
    // Wait for P4 ready again, then give it time to activate the plugin.
    uint32_t deadline_us = get_time_us() + (100 * 1000);
    while (!gpio_read(CTL_RDY_PIN)) {
        if ((int32_t)(get_time_us() - deadline_us) >= 0) break;
    }
    delay_us(10000);  // 10ms for P4 to execute (matches reference)

    printf("[P4-CTL] SetActivePlugin sent OK\n");
    return 1;
}
