/*
 * PIO-based SPI master for OLED display (TX-only, Mode 0).
 *
 * Uses pio2 (pio0 reserved for future use, pio1 for I2S audio).
 * Pins: SCK=GPIO14, MOSI=GPIO15 (same physical pins as hardware spi1,
 * but driven by PIO so spi1 remains free for P4 real-time link).
 *
 * Provides a drop-in replacement for spi_write_blocking().
 */
#ifndef PIO_SPI_OLED_H
#define PIO_SPI_OLED_H

#include <stdint.h>
#include <stddef.h>
#include "hardware/pio.h"

/* Initialize PIO SPI for OLED on pio2, SCK=GPIO14, MOSI=GPIO15.
 * Must be called before any oled_spi_write_blocking() calls. */
void oled_spi_init(void);

/* Write len bytes over PIO SPI (blocking). Drop-in for spi_write_blocking(). */
void oled_spi_write_blocking(const uint8_t *src, size_t len);

#endif /* PIO_SPI_OLED_H */
