/*
 * SD Card WAD Loader for TBD-16 Doom
 *
 * Reads doom1.whx from SD card into PSRAM at boot.
 * Uses Petit FatFS (SPI mode on SPI0) — same approach as uf2loader.
 * After loading, the WAD is memory-mapped in PSRAM at 0x11000000
 * and whd_map_base is pointed there for zero-copy lump access.
 *
 * SD card pins (SPI mode): SCK=GPIO2, MOSI=GPIO3, MISO=GPIO4, CS=GPIO7
 * SD power enable: GPIO17 (handled by diskio.c disk_initialize, same as uf2loader)
 * PSRAM: CS1 on GPIO19, mapped at 0x11000000
 */

#if USE_SD_WAD

#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include <stdio.h>

#include "hardware/gpio.h"
#include "pico/platform.h"
#include "pff.h"

/* PSRAM base address after QMI CS1 init */
#define PSRAM_BASE_ADDR  0x11000000

/* WAD file path on SD card (UPPERCASE required — Petit FatFS with PF_USE_LCC=0) */
#define SD_WAD_PATH   "/DATA/DOOM1.WHX"

/* Read buffer size for pf_read (must fit in stack) */
#define READ_CHUNK_SIZE  4096

/* External: PSRAM init returns size, or 0 on failure */
extern size_t psram_init(uint cs_pin);

/* External: call this to set whd_map_base and fileo.mapped */
extern void sd_wad_set_base(const uint8_t *base);

/* External: Petit FatFS disk deinit (frees SPI0 pins for other use) */
extern void disk_deinitialize(void);

int sd_wad_load(void) {
    FATFS fs;
    UINT bytes_read;
    uint32_t total_read = 0;
    uint8_t *psram = (uint8_t *)PSRAM_BASE_ADDR;

    printf("[SD-WAD] Initializing PSRAM on GPIO%d...\n", J_PSRAM_CS);
    size_t psram_size = psram_init(J_PSRAM_CS);
    if (!psram_size) {
        printf("[SD-WAD] ERROR: PSRAM not detected!\n");
        return -1;
    }
    printf("[SD-WAD] PSRAM: %u MB detected\n", (unsigned)(psram_size / (1024 * 1024)));

    /* SD card power cycling is handled by disk_initialize() inside pf_mount(),
       exactly as the uf2loader does it (GPIO17 power enable). */
    printf("[SD-WAD] Mounting FAT filesystem...\n");
    FRESULT res = pf_mount(&fs);
    if (res != FR_OK) {
        printf("[SD-WAD] ERROR: pf_mount failed (%d)\n", res);
        return -2;
    }

    printf("[SD-WAD] Opening %s...\n", SD_WAD_PATH);
    res = pf_open(SD_WAD_PATH);
    if (res != FR_OK) {
        printf("[SD-WAD] ERROR: pf_open failed (%d)\n", res);
        return -3;
    }

    printf("[SD-WAD] Reading WAD into PSRAM at 0x%08X...\n", PSRAM_BASE_ADDR);

    /* Read the WAD directly into PSRAM in chunks */
    do {
        res = pf_read(psram + total_read, READ_CHUNK_SIZE, &bytes_read);
        if (res != FR_OK) {
            printf("[SD-WAD] ERROR: pf_read failed at offset %lu (%d)\n",
                   (unsigned long)total_read, res);
            return -4;
        }
        total_read += bytes_read;

        /* Progress every 256KB */
        if ((total_read & 0x3FFFF) == 0 && total_read > 0) {
            printf("[SD-WAD]   %lu KB loaded...\n", (unsigned long)(total_read / 1024));
        }
    } while (bytes_read == READ_CHUNK_SIZE);

    printf("[SD-WAD] Loaded %lu bytes (%lu KB)\n",
           (unsigned long)total_read, (unsigned long)(total_read / 1024));

    /* Deinit SD card SPI — free pins for potential SDIO use later */
    disk_deinitialize();

    /* Validate WAD magic */
    if (total_read < 4 ||
        psram[0] != 'I' || psram[1] != 'W' || psram[2] != 'H' || psram[3] != 'X') {
        printf("[SD-WAD] ERROR: Invalid WAD magic at PSRAM (got 0x%02X%02X%02X%02X)\n",
               psram[0], psram[1], psram[2], psram[3]);
        return -5;
    }

    /* Point whd_map_base to the WAD in PSRAM */
    sd_wad_set_base(psram);
    printf("[SD-WAD] WAD loaded OK, whd_map_base = 0x%08X\n", PSRAM_BASE_ADDR);

    return 0;
}

#endif /* USE_SD_WAD */
