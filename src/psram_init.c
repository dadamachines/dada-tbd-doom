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

#include <stdio.h>          // MUST be first — avoids PSRAM init hang on RP2350
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

static size_t __no_inline_not_in_flash_func(psram_detect)(void) {
    int psram_size = 0;

    /* Try and read the PSRAM ID via direct_csr */
    qmi_hw->direct_csr = 30 << QMI_DIRECT_CSR_CLKDIV_LSB | QMI_DIRECT_CSR_EN_BITS;

    /* Wait for cooldown on last XIP transfer */
    while ((qmi_hw->direct_csr & QMI_DIRECT_CSR_BUSY_BITS) != 0) {
    }

    /* Exit out of QMI in case we've inited already */
    qmi_hw->direct_csr |= QMI_DIRECT_CSR_ASSERT_CS1N_BITS;

    /* Transmit as quad */
    qmi_hw->direct_tx = QMI_DIRECT_TX_OE_BITS |
                         QMI_DIRECT_TX_IWIDTH_VALUE_Q << QMI_DIRECT_TX_IWIDTH_LSB |
                         0xf5;

    while ((qmi_hw->direct_csr & QMI_DIRECT_CSR_BUSY_BITS) != 0) {
    }

    (void)qmi_hw->direct_rx;

    qmi_hw->direct_csr &= ~(QMI_DIRECT_CSR_ASSERT_CS1N_BITS);

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

        while ((qmi_hw->direct_csr & QMI_DIRECT_CSR_TXEMPTY_BITS) == 0) {
        }

        while ((qmi_hw->direct_csr & QMI_DIRECT_CSR_BUSY_BITS) != 0) {
        }

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
    gpio_set_function(cs_pin, GPIO_FUNC_XIP_CS1);

    uint32_t intr_stash = save_and_disable_interrupts();

    size_t psram_size = psram_detect();

    if (!psram_size) {
        restore_interrupts(intr_stash);
        return 0;
    }

    /* Enable direct mode, PSRAM CS, clkdiv of 10 */
    qmi_hw->direct_csr = 10 << QMI_DIRECT_CSR_CLKDIV_LSB |
                          QMI_DIRECT_CSR_EN_BITS |
                          QMI_DIRECT_CSR_AUTO_CS1N_BITS;
    while (qmi_hw->direct_csr & QMI_DIRECT_CSR_BUSY_BITS) {
    }

    /* Enable QPI mode on the PSRAM */
    qmi_hw->direct_tx = QMI_DIRECT_TX_NOPUSH_BITS | 0x35;

    while (qmi_hw->direct_csr & QMI_DIRECT_CSR_BUSY_BITS) {
    }

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
