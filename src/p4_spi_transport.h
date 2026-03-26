// P4 SPI Transport — picosdk-native real-time audio sender
// Pure pico SDK replacement for the Arduino Midi.cpp / DaDa_SPI transport.
// Link1 (spi1rp ↔ SPI2p4): high-speed audio, 30 MHz, GPIO 28-31.
// Completely independent of spi0 (used by SD card on GPIO 2-7).

#ifndef P4_SPI_TRANSPORT_H
#define P4_SPI_TRANSPORT_H

#ifdef __cplusplus
extern "C" {
#endif

// Initialize spi1 + GPIO for P4 real-time link.
// Call once after pab_init().
void p4_spi_transport_init(void);

// Call from the main loop (or a timer). Checks the word-clock
// counter and, when a new block is due, packs + sends one frame.
void p4_spi_transport_poll(void);

#ifdef __cplusplus
}
#endif

#endif // P4_SPI_TRANSPORT_H
