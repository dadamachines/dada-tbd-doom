/*
 * Hardware configuration for no-OS-FatFS-SD-SDIO-SPI-RPi-Pico library.
 * TBD-16 SD card: SPI mode via hardware spi0 on GPIOs 2-4,7.
 *
 * SD card wiring (active during boot only — spi0 later reused for P4 link):
 *   SCK  = GPIO 2  (SDIO_CLK pad)
 *   MOSI = GPIO 3  (SDIO_CMD pad)
 *   MISO = GPIO 4  (SDIO_D0  pad)
 *   CS   = GPIO 7  (SDIO_D3  pad — active low)
 *   DET  = GPIO 8  (active low card detect)
 *   PWR  = GPIO 17 (active low power enable)
 */

#include "hw_config.h"

static spi_t spi = {
    .hw_inst = spi0,
    .sck_gpio = 2,
    .mosi_gpio = 3,
    .miso_gpio = 4,
    .baud_rate = 125 * 1000 * 1000 / 4,  // 31.25 MHz
};

static sd_spi_if_t spi_if = {
    .spi = &spi,
    .ss_gpio = 7,
};

static sd_card_t sd_card = {
    .type = SD_IF_SPI,
    .spi_if_p = &spi_if,
    .use_card_detect = true,
    .card_detect_gpio = 8,
    .card_detected_true = 0,  // Active low: GPIO reads 0 when card present
    .card_detect_use_pull = true,
    .card_detect_pull_hi = true,
};

size_t sd_get_num(void) { return 1; }

sd_card_t *sd_get_by_num(size_t num) {
    return (num == 0) ? &sd_card : NULL;
}

/* FatFs RTC callback — we have no RTC, return a fixed timestamp */
#include "ff.h"
DWORD get_fattime(void) {
    return ((DWORD)(2025 - 1980) << 25) | ((DWORD)1 << 21) | ((DWORD)1 << 16);
}

/*
 * Stub for SDIO driver function referenced by sd_card.c's switch-case.
 * Never called because our sd_card.type == SD_IF_SPI.
 */
void sd_sdio_ctor(sd_card_t *sd_card_p) { (void)sd_card_p; }

/* Stub for crash.c's capture_assert (we don't include crash.c) */
void capture_assert(const char *file, int line, const char *func, const char *pred) {
    (void)file; (void)line; (void)func; (void)pred;
}
