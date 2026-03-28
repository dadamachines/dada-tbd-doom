/*
 * PSRAM initialization for RP2350 (APS6404 on QMI CS1)
 *
 * Extracted from earlephilhower/arduino-pico psram.cpp
 * Originally from MicroPython project (MIT License)
 * Copyright (c) 2025 Phil Howard, Mike Bell, Kirk D. Benell
 *
 * Stripped down to pure Pico SDK C — no Arduino deps, no TLSF heap.
 * Only provides psram_init(cs_pin) which configures QMI CS1 and returns
 * the detected PSRAM size.  After init, PSRAM is memory-mapped at
 * XIP_BASE + 0x01000000 (0x11000000) and is directly read/writable.
 */

#include <stdio.h>
#include <stdint.h>
#include <stddef.h>
#include "hardware/address_mapped.h"
#include "hardware/clocks.h"
#include "hardware/gpio.h"
#include "hardware/regs/addressmap.h"
#include "hardware/structs/qmi.h"
#include "hardware/structs/xip_ctrl.h"
#include "hardware/sync.h"
#include "pico/platform.h"

/* PSRAM is mapped at this address after init */
#define PSRAM_BASE (XIP_BASE + 0x01000000)  /* 0x11000000 */

/* Expected KGD byte for APS6404 */
#define PSRAM_KGD 0x5D

/* Max SCK frequency for PSRAM */
#ifndef RP2350_PSRAM_MAX_SCK_HZ
#define RP2350_PSRAM_MAX_SCK_HZ (109000000)
#endif

/* Busy-wait with timeout (runs from RAM, no flash access).
 * Returns 0 if busy cleared, 1 if timed out. */
static int __no_inline_not_in_flash_func(qmi_wait_busy)(int timeout) {
    for (int i = 0; i < timeout; i++) {
        if ((qmi_hw->direct_csr & QMI_DIRECT_CSR_BUSY_BITS) == 0)
            return 0;
    }
    return 1;  /* timed out */
}

/* Wait for TX FIFO empty with timeout. Returns 0 if ok, 1 if timed out. */
static int __no_inline_not_in_flash_func(qmi_wait_txempty)(int timeout) {
    for (int i = 0; i < timeout; i++) {
        if (qmi_hw->direct_csr & QMI_DIRECT_CSR_TXEMPTY_BITS)
            return 0;
    }
    return 1;  /* timed out */
}

/* Drain any stale data from the QMI RX FIFO (max 16 reads to prevent hang) */
static void __no_inline_not_in_flash_func(qmi_drain_rx)(void) {
    for (int i = 0; i < 16 && !(qmi_hw->direct_csr & QMI_DIRECT_CSR_RXEMPTY_BITS); i++)
        (void)qmi_hw->direct_rx;
}

/* RAM-safe delay: ~1µs per 150 iterations at 150 MHz */
static void __no_inline_not_in_flash_func(psram_delay_us)(int us) {
    for (volatile int d = 0; d < us * 150; d++) {}
}

/* Send a single SPI command to CS1 (no response expected) */
static void __no_inline_not_in_flash_func(psram_cmd_spi)(uint8_t cmd) {
    qmi_hw->direct_csr |= QMI_DIRECT_CSR_ASSERT_CS1N_BITS;
    qmi_hw->direct_tx = QMI_DIRECT_TX_NOPUSH_BITS | cmd;
    qmi_wait_busy(100000);
    qmi_hw->direct_csr &= ~QMI_DIRECT_CSR_ASSERT_CS1N_BITS;
}

/* Send a single QPI command to CS1 (quad-width, no response expected) */
static void __no_inline_not_in_flash_func(psram_cmd_qpi)(uint8_t cmd) {
    qmi_hw->direct_csr |= QMI_DIRECT_CSR_ASSERT_CS1N_BITS;
    qmi_hw->direct_tx = QMI_DIRECT_TX_OE_BITS |
                         QMI_DIRECT_TX_NOPUSH_BITS |
                         QMI_DIRECT_TX_IWIDTH_VALUE_Q << QMI_DIRECT_TX_IWIDTH_LSB |
                         cmd;
    qmi_wait_busy(100000);
    qmi_hw->direct_csr &= ~QMI_DIRECT_CSR_ASSERT_CS1N_BITS;
}

static size_t __no_inline_not_in_flash_func(psram_detect)(void) {
    int psram_size = 0;

    /* Force-clear direct mode in case it was left enabled from a previous
     * aborted boot (QMI is NOT reset by SYSRESETREQ). */
    qmi_hw->direct_csr = 0;

    /* Clear stale M[1] XIP configuration from previous boot.
     * Without this, the QMI may try to complete an old XIP transfer
     * when we enter direct mode, causing BUSY to stick. */
    qmi_hw->m[1].timing = QMI_M1_TIMING_RESET;
    qmi_hw->m[1].rfmt = 0;
    qmi_hw->m[1].rcmd = 0;
    qmi_hw->m[1].wfmt = 0;
    qmi_hw->m[1].wcmd = 0;

    /* Enter direct mode with slow clock for reliability */
    qmi_hw->direct_csr = 30 << QMI_DIRECT_CSR_CLKDIV_LSB | QMI_DIRECT_CSR_EN_BITS;

    /* Wait for cooldown on last XIP transfer (with timeout) */
    if (qmi_wait_busy(100000)) {
        qmi_hw->direct_csr = 0;
        return 0;
    }

    /* === Warm-boot recovery for APS6404 ===
     * After debug-probe reset the RP2350 resets but the PSRAM retains
     * its state (may be in QPI mode, mid-transaction, or continuous read).
     *
     * Strategy: we don't know if it's in SPI or QPI mode, so we reset
     * in BOTH modes. Reset Enable (0x66) + Reset (0x99) work in both
     * SPI and QPI modes per the APS6404 datasheet. After reset, the
     * device always returns to SPI mode.
     *
     *   1. Drain RX FIFO
     *   2. CS toggle + 32 dummy 0xFF in quad mode (flush any mid-transaction)
     *   3. CS toggle
     *   4. Reset Enable (0x66) in QPI mode
     *   5. Reset (0x99) in QPI mode
     *   6. Wait tRST (200µs)
     *   7. Reset Enable (0x66) in SPI mode
     *   8. Reset (0x99) in SPI mode
     *   9. Wait tRST (200µs)
     */

    /* 1. Drain stale RX data */
    qmi_drain_rx();

    /* 2-3. Abort any mid-transaction with CS toggle + dummy clocks.
     * Send with OE + QUAD width so all 4 SIO pins are driven HIGH —
     * this properly flushes QPI-mode transactions (not just SPI). */
    qmi_hw->direct_csr |= QMI_DIRECT_CSR_ASSERT_CS1N_BITS;
    for (int i = 0; i < 32; i++) {
        qmi_hw->direct_tx = QMI_DIRECT_TX_OE_BITS |
                             QMI_DIRECT_TX_NOPUSH_BITS |
                             QMI_DIRECT_TX_IWIDTH_VALUE_Q << QMI_DIRECT_TX_IWIDTH_LSB |
                             0xFF;
        qmi_wait_busy(100000);
    }
    qmi_drain_rx();
    qmi_hw->direct_csr &= ~QMI_DIRECT_CSR_ASSERT_CS1N_BITS;
    psram_delay_us(50);

    /* 4-5. Reset in QPI mode (in case PSRAM is in QPI) */
    psram_cmd_qpi(0x66);  /* Reset Enable */
    psram_cmd_qpi(0x99);  /* Reset */

    /* 6. Wait for reset to complete */
    psram_delay_us(200);

    /* 7-8. Reset in SPI mode (in case PSRAM was in SPI, or QPI reset worked) */
    psram_cmd_spi(0x66);  /* Reset Enable */
    psram_cmd_spi(0x99);  /* Reset */

    /* 9. Wait for reset to complete */
    psram_delay_us(200);

    /* Final drain before ID read */
    qmi_drain_rx();

    /* Read the ID */
    qmi_hw->direct_csr |= QMI_DIRECT_CSR_ASSERT_CS1N_BITS;
    uint8_t kgd = 0;
    uint8_t eid = 0;

    for (size_t i = 0; i < 7; i++) {
        if (i == 0) {
            qmi_hw->direct_tx = 0x9f;
        } else {
            qmi_hw->direct_tx = 0xff;
        }

        if (qmi_wait_txempty(100000)) {
            qmi_hw->direct_csr &= ~(QMI_DIRECT_CSR_ASSERT_CS1N_BITS | QMI_DIRECT_CSR_EN_BITS);
            return 0;  /* TX stuck — bail out, will retry */
        }

        qmi_wait_busy(100000);

        if (i == 5) {
            kgd = qmi_hw->direct_rx;
        } else if (i == 6) {
            eid = qmi_hw->direct_rx;
        } else {
            (void)qmi_hw->direct_rx;
        }
    }

    /* Disable direct csr */
    qmi_hw->direct_csr &= ~(QMI_DIRECT_CSR_ASSERT_CS1N_BITS | QMI_DIRECT_CSR_EN_BITS);

    if (kgd == PSRAM_KGD) {
        psram_size = 1024 * 1024; /* 1 MiB */
        uint8_t size_id = eid >> 5;
        if (eid == 0x26 || size_id == 2) {
            psram_size *= 8; /* 8 MiB */
        } else if (size_id == 0) {
            psram_size *= 2; /* 2 MiB */
        } else if (size_id == 1) {
            psram_size *= 4; /* 4 MiB */
        }
    }

    return psram_size;
}

size_t __no_inline_not_in_flash_func(psram_init)(uint cs_pin) {
    /* Drive CS# HIGH via SIO before handing to QMI.
     * After debug-probe reset, GPIO19 may still be XIP_CS1 function
     * with stale QMI config — switch to SIO output HIGH first to
     * give PSRAM a clean CS# deassert.
     * Note: set output value BEFORE direction to avoid a LOW glitch. */
    gpio_set_function(cs_pin, GPIO_FUNC_SIO);  /* switch away from XIP */
    gpio_put(cs_pin, 1);                       /* output latch = HIGH */
    gpio_set_dir(cs_pin, GPIO_OUT);            /* now driving HIGH */
    psram_delay_us(500);  /* 500us with CS# HIGH — PSRAM sees clean deassert */

    gpio_set_function(cs_pin, GPIO_FUNC_XIP_CS1);

    /* Retry detection up to 5 times with 10ms delays between attempts.
     * Each attempt gets its own interrupt-disabled window for QMI access;
     * delays run with interrupts enabled for real-time accuracy. */
    size_t psram_size = 0;
    for (int attempt = 0; attempt < 5 && !psram_size; attempt++) {
        if (attempt > 0) {
            printf("[PSRAM] attempt %d failed, retrying...\n", attempt);
            psram_delay_us(10000);  /* 10ms between retries */
        }
        uint32_t intr_stash = save_and_disable_interrupts();
        psram_size = psram_detect();
        restore_interrupts(intr_stash);
    }

    if (!psram_size)
        return 0;

    uint32_t intr_stash = save_and_disable_interrupts();

    /* Enable direct mode, PSRAM CS, clkdiv of 10 */
    qmi_hw->direct_csr = 10 << QMI_DIRECT_CSR_CLKDIV_LSB |
                          QMI_DIRECT_CSR_EN_BITS |
                          QMI_DIRECT_CSR_AUTO_CS1N_BITS;
    qmi_wait_busy(100000);

    /* Enable QPI mode on the PSRAM */
    qmi_hw->direct_tx = QMI_DIRECT_TX_NOPUSH_BITS | 0x35;

    qmi_wait_busy(100000);

    /* Calculate timing parameters */
    const int max_psram_freq = RP2350_PSRAM_MAX_SCK_HZ;
    const int clock_hz = clock_get_hz(clk_sys);
    int divisor = (clock_hz + max_psram_freq - 1) / max_psram_freq;
    if (divisor == 1 && clock_hz > 100000000) {
        divisor = 2;
    }
    int rxdelay = divisor;
    if (clock_hz / divisor > 100000000) {
        rxdelay += 1;
    }

    /* Max select <= 8us (in multiples of 64 sys clocks) */
    /* Min deselect >= 18ns (in sys clock cycles - ceil(divisor/2)) */
    const int clock_period_fs = 1000000000000000ll / clock_hz;
    const int max_select = (125 * 1000000) / clock_period_fs;
    const int min_deselect = (18 * 1000000 + (clock_period_fs - 1)) / clock_period_fs - (divisor + 1) / 2;

    qmi_hw->m[1].timing = 1 << QMI_M1_TIMING_COOLDOWN_LSB |
                           QMI_M1_TIMING_PAGEBREAK_VALUE_1024 << QMI_M1_TIMING_PAGEBREAK_LSB |
                           max_select << QMI_M1_TIMING_MAX_SELECT_LSB |
                           min_deselect << QMI_M1_TIMING_MIN_DESELECT_LSB |
                           rxdelay << QMI_M1_TIMING_RXDELAY_LSB |
                           divisor << QMI_M1_TIMING_CLKDIV_LSB;

    /* Set PSRAM read command: quad mode, cmd 0xEB, 6 dummy cycles */
    qmi_hw->m[1].rfmt =
        QMI_M0_RFMT_PREFIX_WIDTH_VALUE_Q << QMI_M0_RFMT_PREFIX_WIDTH_LSB |
        QMI_M0_RFMT_ADDR_WIDTH_VALUE_Q << QMI_M0_RFMT_ADDR_WIDTH_LSB |
        QMI_M0_RFMT_SUFFIX_WIDTH_VALUE_Q << QMI_M0_RFMT_SUFFIX_WIDTH_LSB |
        QMI_M0_RFMT_DUMMY_WIDTH_VALUE_Q << QMI_M0_RFMT_DUMMY_WIDTH_LSB |
        QMI_M0_RFMT_DATA_WIDTH_VALUE_Q << QMI_M0_RFMT_DATA_WIDTH_LSB |
        QMI_M0_RFMT_PREFIX_LEN_VALUE_8 << QMI_M0_RFMT_PREFIX_LEN_LSB |
        6 << QMI_M0_RFMT_DUMMY_LEN_LSB;

    qmi_hw->m[1].rcmd = 0xEB;

    /* Set PSRAM write command: quad mode, cmd 0x38 */
    qmi_hw->m[1].wfmt =
        QMI_M0_WFMT_PREFIX_WIDTH_VALUE_Q << QMI_M0_WFMT_PREFIX_WIDTH_LSB |
        QMI_M0_WFMT_ADDR_WIDTH_VALUE_Q << QMI_M0_WFMT_ADDR_WIDTH_LSB |
        QMI_M0_WFMT_SUFFIX_WIDTH_VALUE_Q << QMI_M0_WFMT_SUFFIX_WIDTH_LSB |
        QMI_M0_WFMT_DUMMY_WIDTH_VALUE_Q << QMI_M0_WFMT_DUMMY_WIDTH_LSB |
        QMI_M0_WFMT_DATA_WIDTH_VALUE_Q << QMI_M0_WFMT_DATA_WIDTH_LSB |
        QMI_M0_WFMT_PREFIX_LEN_VALUE_8 << QMI_M0_WFMT_PREFIX_LEN_LSB;

    qmi_hw->m[1].wcmd = 0x38;

    /* Disable direct mode */
    qmi_hw->direct_csr = 0;

    /* Enable writes to PSRAM via XIP */
    hw_set_bits(&xip_ctrl_hw->ctrl, XIP_CTRL_WRITABLE_M1_BITS);

    restore_interrupts(intr_stash);

    return psram_size;
}
