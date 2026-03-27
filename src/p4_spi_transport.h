// P4 SPI Transport — blocking DMA audio sender (matching DaDa_SPI pattern)
// Link1 (spi1rp ↔ SPI2p4): high-speed audio, ~37.5 MHz, GPIO 28-31.
// Completely independent of spi0 (used by SD card on GPIO 2-7).
//
// Uses direct register access (no SDK headers) to avoid the RP2350
// PSRAM init hang triggered by stdbool.h include order issues.

#ifndef P4_SPI_TRANSPORT_H
#define P4_SPI_TRANSPORT_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// Initialize spi1 + GPIO + DMA for P4 real-time link.
// Call once after pab_init().
void p4_spi_transport_init(void);

// Send as many DMA frames as the time budget allows (~15 ms).
// Call from the game loop after filling the ring buffer.
void p4_spi_transport_poll(void);

#ifdef __cplusplus
}
#endif

#endif // P4_SPI_TRANSPORT_H
