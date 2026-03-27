// TBD-16 Doom — PlatformIO entry point
// The actual main() is in lib/rp2040-doom/src/i_main.c (added via doom_build.py)
// This file exists because PlatformIO requires at least one source in src/
// It is intentionally empty — do not add main() here.

#include <stdio.h>          // MUST be first — avoids PSRAM init hang on RP2350
#include <stdint.h>
#include <stdbool.h>
#include <stdarg.h>
#include "hardware/uart.h"
#include "hardware/gpio.h"

// --------------------------------------------------------------------------
// Lightweight UART1 printf — bypasses SDK pico_stdio (which crashes because
// newlib buffered I/O calls malloc → Z_Malloc).  Writes directly to UART1
// hardware on GPIO20, connected to the Pico Debug Probe.
// --------------------------------------------------------------------------

#define DBG_UART uart1
#define DBG_TX_PIN 20
#define DBG_BAUD 115200

static volatile bool dbg_uart_ready = false;

void debug_uart_init(void) {
    uart_init(DBG_UART, DBG_BAUD);
    gpio_set_function(DBG_TX_PIN, GPIO_FUNC_UART);
    dbg_uart_ready = true;
}

static void dbg_puts_raw(const char *s) {
    if (!dbg_uart_ready) return;
    while (*s) {
        if (*s == '\n') uart_putc_raw(DBG_UART, '\r');
        uart_putc_raw(DBG_UART, *s++);
    }
}

// Tiny vprintf that writes directly to UART1 (no malloc, no newlib)
static int dbg_vprintf(const char *fmt, va_list args) {
    if (!dbg_uart_ready) return 0;
    char buf[256];
    int n = vsnprintf(buf, sizeof(buf), fmt, args);
    dbg_puts_raw(buf);
    return n;
}

int __wrap_printf(const char *fmt, ...) {
    va_list args;
    va_start(args, fmt);
    int n = dbg_vprintf(fmt, args);
    va_end(args);
    return n;
}
int __wrap_vprintf(const char *fmt, va_list args) { return dbg_vprintf(fmt, args); }
int __wrap_puts(const char *s) { dbg_puts_raw(s); dbg_puts_raw("\n"); return 0; }
int __wrap_putchar(int c) {
    if (!dbg_uart_ready) return c;
    if (c == '\n') uart_putc_raw(DBG_UART, '\r');
    uart_putc_raw(DBG_UART, (char)c);
    return c;
}
int __wrap_fprintf(FILE *f, const char *fmt, ...) {
    (void)f;
    va_list args;
    va_start(args, fmt);
    int n = dbg_vprintf(fmt, args);
    va_end(args);
    return n;
}

// Stubs for pico alarm pool (referenced by stdio_usb but not needed)
typedef struct alarm_pool alarm_pool_t;
typedef int64_t (*alarm_callback_t)(int32_t id, void *user_data);
alarm_pool_t *alarm_pool_get_default(void) { return 0; }
int32_t add_alarm_in_us(uint64_t us, alarm_callback_t callback, void *user_data, bool fire_if_past) {
    (void)us; (void)callback; (void)user_data; (void)fire_if_past;
    return -1;
}

// Stub for TinyUSB HID host callback (referenced by hid_host.c)
void tuh_hid_report_received_cb(uint8_t dev_addr, uint8_t instance, uint8_t const *report, uint16_t len) {
    (void)dev_addr; (void)instance; (void)report; (void)len;
}
