/*
 * PIO-based SPI master for OLED display (TX-only, Mode 0).
 *
 * Uses pio0 (SM 0) for SPI clocking.
 * Pins: SCK=GPIO14, MOSI=GPIO15 (same physical pins as hardware spi1,
 * but driven by PIO so spi1 remains free for P4 real-time link).
 *
 * Provides blocking (CPU) and DMA-driven transfer paths.
 */
#ifndef PIO_SPI_OLED_H
#define PIO_SPI_OLED_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include "hardware/pio.h"

/* Initialize PIO SPI for OLED on pio0, SCK=GPIO14, MOSI=GPIO15.
 * Also claims a DMA channel for frame transfers.
 * Must be called before any oled_spi_* calls. */
void oled_spi_init(void);

/* Write len bytes over PIO SPI (blocking, CPU-driven).
 * Use for small command sequences (command_park, etc.). */
void oled_spi_write_blocking(const uint8_t *src, size_t len);

/* Start a DMA transfer of count uint32_t words to PIO SPI.
 * Each word must be pre-packed as (byte << 24) for MSB-first output.
 * Returns immediately — use oled_spi_dma_wait() to block on completion.
 * Ideal for 1024-byte frame data (pass 1024 words). */
void oled_spi_dma_start(const uint32_t *buf32, size_t count);

/* Check if a DMA transfer is still in progress. */
bool oled_spi_dma_busy(void);

/* Block until the current DMA transfer completes and PIO FIFO drains. */
void oled_spi_dma_wait(void);

#endif /* PIO_SPI_OLED_H */
