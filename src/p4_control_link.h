// P4 Control Link — picosdk-native state API sender
// Link2 (spi0rp ↔ SPI3p4): low-speed control, 30 MHz, GPIO 32-35.
// Used to send SetActivePlugin and other commands to the ESP32-P4.
// Shares spi0 with SD card (GPIO 2-7) — init only AFTER disk_deinitialize().
//
// NOTE: Using int instead of bool to avoid stdbool.h include order issues
// that cause PSRAM init hangs on RP2350.

#ifndef P4_CONTROL_LINK_H
#define P4_CONTROL_LINK_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// Initialize spi0 on GPIO 32-35 for P4 control link.
// Call AFTER SD card has released spi0 (disk_deinitialize).
void p4_control_link_init(void);

// Wait for the P4 to come alive (word-clock pulses on GPIO 27).
// Blocks until at least `min_pulses` WS edges are seen, or timeout_ms expires.
// Returns 1 if P4 is alive, 0 on timeout.
int p4_control_link_wait_alive(uint32_t min_pulses, uint32_t timeout_ms);

// Send SetActivePlugin command to P4.
// Returns 1 if P4 acknowledged with matching fingerprint, 0 on failure.
int p4_control_link_set_active_plugin(uint8_t channel, const char *plugin_name);

#ifdef __cplusplus
}
#endif

#endif // P4_CONTROL_LINK_H
