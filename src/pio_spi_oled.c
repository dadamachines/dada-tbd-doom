/*
 * PIO-based SPI master for OLED display (TX-only, Mode 0).
 *
 * Based on the official Raspberry Pi pico-examples/pio/spi pattern.
 * Uses pio0 state machine 0.
 * SCK = GPIO 14 (side-set pin)
 * MOSI = GPIO 15 (OUT pin)
 */

#include "pio_spi_oled.h"
#include "pio_spi_oled.pio.h"
#include "hardware/pio.h"
#include "hardware/gpio.h"
#include "hardware/clocks.h"
#include "pico/time.h"

#define OLED_PIO      pio0
#define OLED_SM       0
#define OLED_SCK_PIN  14
#define OLED_MOSI_PIN 15

/* Target SPI clock frequency */
#define OLED_SPI_FREQ 8000000  /* 8 MHz */

static uint pio_offset;
static bool pio_spi_initialized = false;

void oled_spi_init(void) {
    if (pio_spi_initialized) return;
    pio_spi_initialized = true;

    /* Load PIO program */
    pio_offset = pio_add_program(OLED_PIO, &pio_spi_oled_program);

    pio_sm_config c = pio_spi_oled_program_get_default_config(pio_offset);

    /* OUT pin: MOSI */
    sm_config_set_out_pins(&c, OLED_MOSI_PIN, 1);

    /* Side-set pin: SCK */
    sm_config_set_sideset_pins(&c, OLED_SCK_PIN);

    /* Autopull: shift 8 bits at a time, MSB first, autopull ON */
    sm_config_set_out_shift(&c, false, true, 8);

    /* Clock divider: 2 PIO cycles per SPI bit */
    float sys_freq = (float)clock_get_hz(clk_sys);
    float clkdiv = sys_freq / (OLED_SPI_FREQ * 2.0f);
    if (clkdiv < 1.0f) clkdiv = 1.0f;
    sm_config_set_clkdiv(&c, clkdiv);

    /* Official pico-examples pattern: set initial pin states BEFORE enabling.
     * SCK=0, MOSI=0; both outputs. */
    pio_sm_set_pins_with_mask(OLED_PIO, OLED_SM, 0,
        (1u << OLED_SCK_PIN) | (1u << OLED_MOSI_PIN));
    pio_sm_set_pindirs_with_mask(OLED_PIO, OLED_SM,
        (1u << OLED_SCK_PIN) | (1u << OLED_MOSI_PIN),
        (1u << OLED_SCK_PIN) | (1u << OLED_MOSI_PIN));

    /* Hand GPIO mux control to PIO */
    pio_gpio_init(OLED_PIO, OLED_SCK_PIN);
    pio_gpio_init(OLED_PIO, OLED_MOSI_PIN);

    /* Initialize and enable state machine */
    pio_sm_init(OLED_PIO, OLED_SM, pio_offset, &c);
    pio_sm_set_enabled(OLED_PIO, OLED_SM, true);
}

void __not_in_flash_func(oled_spi_write_blocking)(const uint8_t *src, size_t len) {
    for (size_t i = 0; i < len; i++) {
        /* MSB-first: data must be left-justified (bits 31..24) */
        pio_sm_put_blocking(OLED_PIO, OLED_SM, (uint32_t)src[i] << 24);
    }
    /* Wait for TX FIFO to drain */
    while (!pio_sm_is_tx_fifo_empty(OLED_PIO, OLED_SM))
        tight_loop_contents();
    /* Wait for last byte to fully clock out (~2 us at 8 MHz) */
    busy_wait_us_32(2);
}
