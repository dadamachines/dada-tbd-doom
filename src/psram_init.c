/*
 * PSRAM initialization for RP2350 (APS6404 on QMI CS1)
 *
 * Extracted from earlephilhower/arduino-pico psram.cpp
 * Originally from MicroPython project (MIT License)
 * Copyright (c) 2025 Phil Howard, Mike Bell, Kirk D. Benell
 *
 * ZERO SDK HEADERS — pure register access only.
 * SDK headers pull in stdbool.h via pico/types.h which causes
 * PSRAM init hangs on RP2350. This version uses only stdio.h,
 * stdint.h, stddef.h and direct register manipulation.
 */

#include <stdio.h>
#include <stdint.h>
#include <stddef.h>

/* ---- Section attribute (replaces pico/platform.h) ---- */
#define _RAM_FUNC(name) \
    __attribute__((noinline, section(".time_critical." #name))) name

/* ---- Register bases ---- */
#define QMI_BASE        0x400d0000u
#define XIP_CTRL_BASE   0x400c8000u
#define IO_BANK0_BASE   0x40028000u
#define SIO_BASE        0xd0000000u

/* ---- Volatile register accessors ---- */
#define REG(addr)       (*(volatile uint32_t *)(addr))

/* ---- QMI registers ---- */
#define QMI_DIRECT_CSR  REG(QMI_BASE + 0x00)
#define QMI_DIRECT_TX   REG(QMI_BASE + 0x04)
#define QMI_DIRECT_RX   REG(QMI_BASE + 0x08)
#define QMI_M0_TIMING   REG(QMI_BASE + 0x0C)
#define QMI_M0_RFMT     REG(QMI_BASE + 0x10)
#define QMI_M0_RCMD     REG(QMI_BASE + 0x14)
#define QMI_M0_WFMT     REG(QMI_BASE + 0x18)
#define QMI_M0_WCMD     REG(QMI_BASE + 0x1C)
#define QMI_M1_TIMING   REG(QMI_BASE + 0x20)
#define QMI_M1_RFMT     REG(QMI_BASE + 0x24)
#define QMI_M1_RCMD     REG(QMI_BASE + 0x28)
#define QMI_M1_WFMT     REG(QMI_BASE + 0x2C)
#define QMI_M1_WCMD     REG(QMI_BASE + 0x30)

/* DIRECT_CSR bits */
#define CSR_BUSY        (1u << 1)
#define CSR_ASSERT_CS1N (1u << 3)
#define CSR_AUTO_CS1N   (1u << 7)
#define CSR_TXEMPTY     (1u << 11)
#define CSR_RXEMPTY     (1u << 16)
#define CSR_EN          (1u << 0)
#define CSR_CLKDIV_LSB  22

/* DIRECT_TX bits */
#define TX_NOPUSH       (1u << 20)
#define TX_OE           (1u << 19)
#define TX_IWIDTH_LSB   16
#define TX_IWIDTH_Q     2u          /* quad */

/* M1_TIMING reset value */
#define M1_TIMING_RESET 0x40000004u

/* M1_TIMING field positions */
#define TIM_COOLDOWN_LSB    30
#define TIM_PAGEBREAK_LSB   28
#define TIM_PAGEBREAK_1024  2u
#define TIM_MAX_SELECT_LSB  17
#define TIM_MIN_DESELECT_LSB 12
#define TIM_RXDELAY_LSB     8
#define TIM_CLKDIV_LSB      0

/* RFMT / WFMT field positions (same layout for M0 and M1) */
#define FMT_PREFIX_WIDTH_LSB  0
#define FMT_ADDR_WIDTH_LSB    2
#define FMT_SUFFIX_WIDTH_LSB  4
#define FMT_DUMMY_WIDTH_LSB   6
#define FMT_DATA_WIDTH_LSB    8
#define FMT_PREFIX_LEN_LSB    12
#define FMT_DUMMY_LEN_LSB     16
#define FMT_WIDTH_Q           2u    /* quad */
#define FMT_PREFIX_LEN_8      1u    /* 8-bit prefix */

/* ---- XIP_CTRL ---- */
#define XIP_CTRL        REG(XIP_CTRL_BASE + 0x00)
/* Atomic SET/CLR aliases: base + 0x2000 for SET, + 0x3000 for CLR */
#define XIP_CTRL_SET    REG(XIP_CTRL_BASE + 0x2000 + 0x00)
#define XIP_CTRL_CLR    REG(XIP_CTRL_BASE + 0x3000 + 0x00)
#define XIP_WRITABLE_M1 (1u << 11)

/* ---- GPIO ---- */
/* GPIO_CTRL register for pin N: IO_BANK0_BASE + N*8 + 4 */
#define GPIO_CTRL(pin)  REG(IO_BANK0_BASE + (pin) * 8 + 4)
#define FUNCSEL_MASK    0x1fu
#define FUNC_SIO        5u
#define FUNC_XIP_CS1    9u

/* SIO registers (GPIO 0-31) */
#define SIO_OUT_SET     REG(SIO_BASE + 0x18)
#define SIO_OUT_CLR     REG(SIO_BASE + 0x20)
#define SIO_OE_SET      REG(SIO_BASE + 0x38)
#define SIO_OE_CLR      REG(SIO_BASE + 0x40)

/* ---- PADS_BANK0 (RP2350: every pad starts ISOLATED after reset!) ---- */
#define PADS_BANK0_BASE 0x40038000u
/* Pad register for GPIO N = PADS_BANK0_BASE + 0x04 + N*4 */
#define PAD_REG(pin)    REG(PADS_BANK0_BASE + 0x04 + (pin) * 4)
/* Atomic SET/CLR aliases */
#define PAD_SET(pin)    REG(PADS_BANK0_BASE + 0x2000 + 0x04 + (pin) * 4)
#define PAD_CLR(pin)    REG(PADS_BANK0_BASE + 0x3000 + 0x04 + (pin) * 4)
/* Pad register bits */
#define PAD_ISO         (1u << 8)   /* isolation — MUST clear or pad is disconnected */
#define PAD_OD          (1u << 7)   /* output disable */
#define PAD_IE          (1u << 6)   /* input enable */

/* ---- Interrupt save/restore (Cortex-M33 PRIMASK) ---- */
static inline uint32_t _save_and_disable_irq(void) {
    uint32_t primask;
    __asm volatile ("mrs %0, PRIMASK\n" "cpsid i" : "=r"(primask) :: "memory");
    return primask;
}
static inline void _restore_irq(uint32_t primask) {
    __asm volatile ("msr PRIMASK, %0" :: "r"(primask) : "memory");
}

/* ---- PSRAM constants ---- */
#define PSRAM_KGD  0x5D

#ifndef RP2350_PSRAM_MAX_SCK_HZ
#define RP2350_PSRAM_MAX_SCK_HZ 109000000
#endif

/* System clock — RP2350 on TBD-16 runs at 150 MHz */
#define SYS_CLK_HZ 150000000

/* ==================================================================
 *  All functions below run from RAM (not flash) via section attribute.
 *  This is critical: during QMI direct mode, flash (M[0]) is blocked.
 * ================================================================== */

static int _RAM_FUNC(qmi_wait_busy)(int timeout) {
    for (int i = 0; i < timeout; i++) {
        if ((QMI_DIRECT_CSR & CSR_BUSY) == 0)
            return 0;
    }
    return 1;
}

static int _RAM_FUNC(qmi_wait_txempty)(int timeout) {
    for (int i = 0; i < timeout; i++) {
        if (QMI_DIRECT_CSR & CSR_TXEMPTY)
            return 0;
    }
    return 1;
}

static void _RAM_FUNC(qmi_drain_rx)(void) {
    for (int i = 0; i < 16 && !(QMI_DIRECT_CSR & CSR_RXEMPTY); i++)
        (void)QMI_DIRECT_RX;
}

static void _RAM_FUNC(psram_delay_us)(int us) {
    for (volatile int d = 0; d < us * 150; d++) {}
}

static void _RAM_FUNC(psram_cmd_spi)(uint8_t cmd) {
    QMI_DIRECT_CSR |= CSR_ASSERT_CS1N;
    QMI_DIRECT_TX = TX_NOPUSH | cmd;
    qmi_wait_busy(100000);
    QMI_DIRECT_CSR &= ~CSR_ASSERT_CS1N;
}

static void _RAM_FUNC(psram_cmd_qpi)(uint8_t cmd) {
    QMI_DIRECT_CSR |= CSR_ASSERT_CS1N;
    QMI_DIRECT_TX = TX_OE | TX_NOPUSH | (TX_IWIDTH_Q << TX_IWIDTH_LSB) | cmd;
    qmi_wait_busy(100000);
    QMI_DIRECT_CSR &= ~CSR_ASSERT_CS1N;
}

static size_t _RAM_FUNC(psram_detect)(void) {
    int psram_size = 0;

    /* Force-clear direct mode */
    QMI_DIRECT_CSR = 0;

    /* Clear stale M[1] XIP config from previous boot */
    QMI_M1_TIMING = M1_TIMING_RESET;
    QMI_M1_RFMT   = 0;
    QMI_M1_RCMD   = 0;
    QMI_M1_WFMT   = 0;
    QMI_M1_WCMD   = 0;

    /* Enter direct mode with slow clock */
    QMI_DIRECT_CSR = (30u << CSR_CLKDIV_LSB) | CSR_EN;

    /* Wait for cooldown */
    if (qmi_wait_busy(100000)) {
        QMI_DIRECT_CSR = 0;
        return 0;
    }

    /* === Warm-boot recovery for APS6404 ===
     * After debug-probe reset the RP2350 resets but PSRAM retains state
     * (may be in QPI mode, mid-transaction, or continuous read).
     * Reset in BOTH QPI and SPI modes to ensure clean state. */

    qmi_drain_rx();

    /* Abort mid-transaction: quad-width dummy clocks */
    QMI_DIRECT_CSR |= CSR_ASSERT_CS1N;
    for (int i = 0; i < 32; i++) {
        QMI_DIRECT_TX = TX_OE | TX_NOPUSH | (TX_IWIDTH_Q << TX_IWIDTH_LSB) | 0xFF;
        qmi_wait_busy(100000);
    }
    qmi_drain_rx();
    QMI_DIRECT_CSR &= ~CSR_ASSERT_CS1N;
    psram_delay_us(50);

    /* Reset in QPI mode */
    psram_cmd_qpi(0x66);  /* Reset Enable */
    psram_cmd_qpi(0x99);  /* Reset */
    psram_delay_us(200);

    /* Reset in SPI mode */
    psram_cmd_spi(0x66);
    psram_cmd_spi(0x99);
    psram_delay_us(200);

    qmi_drain_rx();

    /* Read PSRAM ID (RQID command 0x9F + 3 addr + 2 data) */
    QMI_DIRECT_CSR |= CSR_ASSERT_CS1N;
    uint8_t kgd = 0;
    uint8_t eid = 0;

    for (int i = 0; i < 7; i++) {
        QMI_DIRECT_TX = (i == 0) ? 0x9f : 0xff;

        if (qmi_wait_txempty(100000)) {
            QMI_DIRECT_CSR &= ~(CSR_ASSERT_CS1N | CSR_EN);
            return 0;
        }
        qmi_wait_busy(100000);

        if (i == 5)      kgd = (uint8_t)QMI_DIRECT_RX;
        else if (i == 6) eid = (uint8_t)QMI_DIRECT_RX;
        else             (void)QMI_DIRECT_RX;
    }

    QMI_DIRECT_CSR &= ~(CSR_ASSERT_CS1N | CSR_EN);

    if (kgd == PSRAM_KGD) {
        psram_size = 1024 * 1024;
        uint8_t size_id = eid >> 5;
        if (eid == 0x26 || size_id == 2)
            psram_size *= 8;
        else if (size_id == 0)
            psram_size *= 2;
        else if (size_id == 1)
            psram_size *= 4;
    }

    return psram_size;
}

/* ---- Public API ---- */

size_t _RAM_FUNC(psram_init)(unsigned int cs_pin) {
    /* === CRITICAL: Clear stale QMI M[1] BEFORE connecting GPIO ===
     * QMI is NOT reset by SYSRESETREQ. If GPIO19 connects to XIP_CS1
     * while M[1] has stale rfmt/rcmd, QMI may immediately start a
     * transaction that blocks M[0] (flash), freezing CPU. */
    QMI_DIRECT_CSR = 0;
    QMI_M1_TIMING  = M1_TIMING_RESET;
    QMI_M1_RFMT    = 0;
    QMI_M1_RCMD    = 0;
    QMI_M1_WFMT    = 0;
    QMI_M1_WCMD    = 0;
    XIP_CTRL_CLR   = XIP_WRITABLE_M1;

    /* Configure pad: set IE, clear OD, clear ISO.
     * On RP2350 pads start ISOLATED (ISO=1) after power-on reset —
     * the pin is electrically disconnected until we clear ISO. */
    PAD_SET(cs_pin) = PAD_IE;                      /* enable input */
    PAD_CLR(cs_pin) = PAD_OD | PAD_ISO;            /* enable output, remove isolation */

    /* Switch GPIO to SIO, drive CS# HIGH, hold for 500µs.
     * Set output latch BEFORE direction to avoid LOW glitch. */
    GPIO_CTRL(cs_pin) = FUNC_SIO;                 /* funcsel = SIO */
    SIO_OUT_SET = (1u << cs_pin);                  /* output = HIGH */
    SIO_OE_SET  = (1u << cs_pin);                  /* direction = OUT */
    psram_delay_us(500);

    /* NOW safe to connect — M[1] is blank, pad is live */
    GPIO_CTRL(cs_pin) = FUNC_XIP_CS1;

    /* Retry detection up to 5 times */
    size_t psram_size = 0;
    for (int attempt = 0; attempt < 5 && !psram_size; attempt++) {
        if (attempt > 0) {
            printf("[PSRAM] attempt %d failed, retrying...\n", attempt);
            psram_delay_us(10000);
        }
        uint32_t intr = _save_and_disable_irq();
        psram_size = psram_detect();
        _restore_irq(intr);
    }

    if (!psram_size)
        return 0;

    uint32_t intr = _save_and_disable_irq();

    /* Direct mode for QPI enable */
    QMI_DIRECT_CSR = (10u << CSR_CLKDIV_LSB) | CSR_EN | CSR_AUTO_CS1N;
    qmi_wait_busy(100000);

    /* Enter QPI mode (cmd 0x35) */
    QMI_DIRECT_TX = TX_NOPUSH | 0x35;
    qmi_wait_busy(100000);

    /* Calculate timing */
    const int clock_hz = SYS_CLK_HZ;
    int divisor = (clock_hz + RP2350_PSRAM_MAX_SCK_HZ - 1) / RP2350_PSRAM_MAX_SCK_HZ;
    if (divisor == 1 && clock_hz > 100000000)
        divisor = 2;
    int rxdelay = divisor;
    if (clock_hz / divisor > 100000000)
        rxdelay += 1;

    const int clock_period_fs = 1000000000000000ll / clock_hz;
    const int max_select = (125 * 1000000) / clock_period_fs;
    const int min_deselect = (18 * 1000000 + (clock_period_fs - 1)) / clock_period_fs
                             - (divisor + 1) / 2;

    QMI_M1_TIMING = (1u            << TIM_COOLDOWN_LSB)    |
                    (TIM_PAGEBREAK_1024 << TIM_PAGEBREAK_LSB) |
                    (max_select    << TIM_MAX_SELECT_LSB)   |
                    (min_deselect  << TIM_MIN_DESELECT_LSB) |
                    (rxdelay       << TIM_RXDELAY_LSB)      |
                    (divisor       << TIM_CLKDIV_LSB);

    /* RFMT: quad everything, 8-bit prefix, 6 dummy cycles, cmd 0xEB */
    QMI_M1_RFMT = (FMT_WIDTH_Q      << FMT_PREFIX_WIDTH_LSB) |
                  (FMT_WIDTH_Q      << FMT_ADDR_WIDTH_LSB)   |
                  (FMT_WIDTH_Q      << FMT_SUFFIX_WIDTH_LSB) |
                  (FMT_WIDTH_Q      << FMT_DUMMY_WIDTH_LSB)  |
                  (FMT_WIDTH_Q      << FMT_DATA_WIDTH_LSB)   |
                  (FMT_PREFIX_LEN_8 << FMT_PREFIX_LEN_LSB)   |
                  (6u               << FMT_DUMMY_LEN_LSB);
    QMI_M1_RCMD = 0xEB;

    /* WFMT: quad everything, 8-bit prefix, no dummy, cmd 0x38 */
    QMI_M1_WFMT = (FMT_WIDTH_Q      << FMT_PREFIX_WIDTH_LSB) |
                  (FMT_WIDTH_Q      << FMT_ADDR_WIDTH_LSB)   |
                  (FMT_WIDTH_Q      << FMT_SUFFIX_WIDTH_LSB) |
                  (FMT_WIDTH_Q      << FMT_DUMMY_WIDTH_LSB)  |
                  (FMT_WIDTH_Q      << FMT_DATA_WIDTH_LSB)   |
                  (FMT_PREFIX_LEN_8 << FMT_PREFIX_LEN_LSB);
    QMI_M1_WCMD = 0x38;

    /* Disable direct mode, enable XIP writes to PSRAM */
    QMI_DIRECT_CSR = 0;
    XIP_CTRL_SET   = XIP_WRITABLE_M1;

    _restore_irq(intr);

    return psram_size;
}
