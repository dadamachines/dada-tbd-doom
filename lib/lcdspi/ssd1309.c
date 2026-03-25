#include <stdbool.h>
#include <stdlib.h>
#include <stdarg.h>
#include <string.h>
#include <ctype.h>
#include <stdio.h>

#include <hardware/spi.h>
#include <hardware/gpio.h>
#include "hardware/timer.h"
#include "pico/stdlib.h"
#include "pico/multicore.h"

#include "ssd1309.h"
#include "i2ckbd.h"

// Include font data
#include "fonts/font6x8.h"
unsigned char *MainFont = (unsigned char *) font6x8;

// Display buffer - 1 bit per pixel, organized in pages
static uint8_t display_buffer[SSD1309_WIDTH * SSD1309_HEIGHT / 8];

// Current cursor position and colors
static int current_x = 0, current_y = 0;
static int gui_fcolour = WHITE;
static int gui_bcolour = BLACK;
static int gui_font_width, gui_font_height;
static int _page_start_offset = 0;
static short hres = SSD1309_WIDTH;
static short vres = SSD1309_HEIGHT;
static char s_height;
static char s_width;
static int lcd_char_pos = 0;
static unsigned char lcd_buffer[SSD1309_WIDTH * 3] = {0}; // Buffer for compatibility
static unsigned char scrollbuff[SSD1309_WIDTH * 3];

// Fast SPI functions (compatibility)
void __not_in_flash_func(spi_write_fast)(spi_inst_t *spi, const uint8_t *src, size_t len) {
    for (size_t i = 0; i < len; ++i) {
        while (!spi_is_writable(spi))
            tight_loop_contents();
        spi_get_hw(spi)->dr = (uint32_t) src[i];
    }
}

void __not_in_flash_func(spi_finish)(spi_inst_t *spi) {
    while (spi_is_readable(spi))
        (void) spi_get_hw(spi)->dr;
    while (spi_get_hw(spi)->sr & SPI_SSPSR_BSY_BITS)
        tight_loop_contents();
    while (spi_is_readable(spi))
        (void) spi_get_hw(spi)->dr;
    spi_get_hw(spi)->icr = SPI_SSPICR_RORIC_BITS;
}

// Low-level SPI functions
void ssd1309_spi_init(void) {
    gpio_init(OLED_MOSI);
    gpio_init(OLED_SCLK);
    gpio_init(OLED_CS);
    gpio_init(OLED_DC);
    gpio_init(OLED_RST);

    gpio_set_dir(OLED_MOSI, GPIO_OUT);
    gpio_set_dir(OLED_SCLK, GPIO_OUT);
    gpio_set_dir(OLED_CS, GPIO_OUT);
    gpio_set_dir(OLED_DC, GPIO_OUT);
    gpio_set_dir(OLED_RST, GPIO_OUT);

    spi_init(SSD1309_SPI_INST, SSD1309_SPI_SPEED);
    gpio_set_function(OLED_SCLK, GPIO_FUNC_SPI);
    gpio_set_function(OLED_MOSI, GPIO_FUNC_SPI);

    gpio_put(OLED_CS, 1);
    gpio_put(OLED_RST, 1);
}

void ssd1309_command(uint8_t cmd) {
    gpio_put(OLED_DC, 0);
    gpio_put(OLED_CS, 0);
    spi_write_blocking(SSD1309_SPI_INST, &cmd, 1);
    gpio_put(OLED_CS, 1);
}

bool ssd1309_command_list(const uint8_t *commands, size_t count) {
    gpio_put(OLED_DC, 0);
    gpio_put(OLED_CS, 0);

    for (size_t i = 0; i < count; i++) {
        spi_write_blocking(SSD1309_SPI_INST, &commands[i], 1);
    }

    gpio_put(OLED_CS, 1);
    return true;
}

void ssd1309_data(uint8_t data) {
    gpio_put(OLED_DC, 1);
    gpio_put(OLED_CS, 0);
    spi_write_blocking(SSD1309_SPI_INST, &data, 1);
    gpio_put(OLED_CS, 1);
}

bool ssd1309_init(void) {
    ssd1309_spi_init();

    gpio_put(OLED_RST, 0);
    sleep_ms(10);
    gpio_put(OLED_RST, 1);
    sleep_ms(10);

    _page_start_offset = 0;

    static const uint8_t init[] = {
        0xFD, 0x12,  // command lock
        0xAE,        // display off
        0xD5, 0xA0,  // set display clock divide ratio/oscillator frequency
        0xA8, 0x3F,  // set multiplex ratio
        0xD3, 0x00,  // set display offset
        0x40,        // set display start line
        0xA1,        // set segment re-map
        0xC8,        // set COM output scan direction
        0xDA, 0x12,  // set COM pins hardware configuration
        0x81, 0xDF,  // set contrast control / current control
        0xD9, 0x82,  // set pre-charge period
        0xDB, 0x34,  // set VCOMH deselect level
        0xA4,        // display on
        0xA6,        // normal display
    };

    if (!ssd1309_command_list(init, sizeof(init))) {
        return false;
    }

    sleep_ms(100);
    ssd1309_command(0xAF);

    memset(display_buffer, 0, sizeof(display_buffer));

    gui_font_width = MainFont[0];
    gui_font_height = MainFont[1];

    return true;
}

void ssd1309_clear(void) {
    memset(display_buffer, 0, sizeof(display_buffer));
    current_x = 0;
    current_y = 0;
}

void ssd1309_set_pixel(int x, int y, int color) {
    if (x < 0 || x >= SSD1309_WIDTH || y < 0 || y >= SSD1309_HEIGHT) {
        return;
    }

    int page = y / 8;
    int bit = y % 8;
    int index = page * SSD1309_WIDTH + x;

    if (color) {
        display_buffer[index] |= (1 << bit);
    } else {
        display_buffer[index] &= ~(1 << bit);
    }
}

void ssd1309_display(void) {
    ssd1309_command(SSD1309_MEMORYMODE);
    ssd1309_command(0x02);

    for (int page = 0; page < 8; page++) {
        ssd1309_command(0xB0 + page);
        ssd1309_command(SSD1309_SETLOWCOLUMN | (_page_start_offset & 0x0F));
        ssd1309_command(SSD1309_SETHIGHCOLUMN | ((_page_start_offset >> 4) & 0x0F));

        gpio_put(OLED_DC, 1);
        gpio_put(OLED_CS, 0);
        spi_write_blocking(SSD1309_SPI_INST, &display_buffer[page * SSD1309_WIDTH], SSD1309_WIDTH);
        gpio_put(OLED_CS, 1);
    }
}

void ssd1309_draw_rect(int x1, int y1, int x2, int y2, int color) {
    // Ensure coordinates are in correct order
    if (x1 > x2) { int temp = x1; x1 = x2; x2 = temp; }
    if (y1 > y2) { int temp = y1; y1 = y2; y2 = temp; }

    // Clamp to display bounds
    if (x1 < 0) x1 = 0;
    if (y1 < 0) y1 = 0;
    if (x2 >= SSD1309_WIDTH) x2 = SSD1309_WIDTH - 1;
    if (y2 >= SSD1309_HEIGHT) y2 = SSD1309_HEIGHT - 1;

    for (int y = y1; y <= y2; y++) {
        for (int x = x1; x <= x2; x++) {
            ssd1309_set_pixel(x, y, color);
        }
    }
}

void ssd1309_draw_line(int x1, int y1, int x2, int y2, int color) {
    int dx = abs(x2 - x1), sx = x1 < x2 ? 1 : -1;
    int dy = abs(y2 - y1), sy = y1 < y2 ? 1 : -1;
    int err = (dx > dy ? dx : -dy) / 2, e2;

    while (1) {
        ssd1309_set_pixel(x1, y1, color);
        if (x1 == x2 && y1 == y2) break;
        e2 = err;
        if (e2 > -dx) { err -= dy; x1 += sx; }
        if (e2 < dy) { err += dx; y1 += sy; }
    }
}

void ssd1309_set_cursor(int x, int y) {
    current_x = x;
    current_y = y;
}

void ssd1309_print_char(int fc, int bc, char c, int orientation) {
    unsigned char *fp = (unsigned char *) MainFont;
    int height = fp[1];
    int width = fp[0];

    if (c >= fp[2] && c < fp[2] + fp[3]) {
        unsigned char *bitmap = fp + 4 + (int)(((c - fp[2]) * height * width) / 8);

        for (int i = 0; i < height; i++) {
            for (int j = 0; j < width; j++) {
                int bit_index = i * width + j;
                int byte_index = bit_index / 8;
                int bit_pos = 7 - (bit_index % 8);

                if (bitmap[byte_index] & (1 << bit_pos)) {
                    ssd1309_set_pixel(current_x + j, current_y + i, fc);
                } else if (bc != -1) {
                    ssd1309_set_pixel(current_x + j, current_y + i, bc);
                }
            }
        }
    } else {
        // Character not in font, draw as space
        if (bc != -1) {
            ssd1309_draw_rect(current_x, current_y, current_x + width - 1, current_y + height - 1, bc);
        }
    }

    if (orientation == ORIENT_NORMAL) {
        current_x += width;
    }
}

void ssd1309_print_string(char *s) {
    while (*s) {
        if (*s == '\n') {
            current_x = 0;
            current_y += gui_font_height;
        } else if (*s == '\r') {
            current_x = 0;
        } else {
            ssd1309_print_char(gui_fcolour, gui_bcolour, *s, ORIENT_NORMAL);
        }
        s++;
    }
    ssd1309_display();  // Update display after printing
}

void ssd1309_print_string_color(char *s, int fg, int bg) {
    int old_fg = gui_fcolour;
    int old_bg = gui_bcolour;

    gui_fcolour = fg;
    gui_bcolour = bg;

    ssd1309_print_string(s);

    gui_fcolour = old_fg;
    gui_bcolour = old_bg;
}

// Complete compatibility layer implementation

void set_font(void) {
    gui_font_width = MainFont[0];
    gui_font_height = MainFont[1];
    s_height = vres / gui_font_height;
    s_width = hres / gui_font_width;
}

void define_region_spi(int xstart, int ystart, int xend, int yend, int rw) {
    // For OLED, this is a no-op as we use a framebuffer approach
    // Just ensure coordinates are valid
    if (xstart < 0) xstart = 0;
    if (ystart < 0) ystart = 0;
    if (xend >= hres) xend = hres - 1;
    if (yend >= vres) yend = vres - 1;
}

void read_buffer_spi(int x1, int y1, int x2, int y2, unsigned char *p) {
    // For monochrome OLED, read from display buffer
    int t;
    if (x2 <= x1) { t = x1; x1 = x2; x2 = t; }
    if (y2 <= y1) { t = y1; y1 = y2; y2 = t; }
    if (x1 < 0) x1 = 0;
    if (x1 >= hres) x1 = hres - 1;
    if (x2 < 0) x2 = 0;
    if (x2 >= hres) x2 = hres - 1;
    if (y1 < 0) y1 = 0;
    if (y1 >= vres) y1 = vres - 1;
    if (y2 < 0) y2 = 0;
    if (y2 >= vres) y2 = vres - 1;

    // Simple implementation - just copy pixel data
    int idx = 0;
    for (int y = y1; y <= y2; y++) {
        for (int x = x1; x <= x2; x++) {
            int page = y / 8;
            int bit = y % 8;
            int buffer_index = page * SSD1309_WIDTH + x;

            if (display_buffer[buffer_index] & (1 << bit)) {
                p[idx] = 0xFF; // White pixel
                p[idx + 1] = 0xFF;
                p[idx + 2] = 0xFF;
            } else {
                p[idx] = 0x00; // Black pixel
                p[idx + 1] = 0x00;
                p[idx + 2] = 0x00;
            }
            idx += 3;
        }
    }
}

void draw_buffer_spi(int x1, int y1, int x2, int y2, unsigned char *p) {
    int t;
    if (x2 <= x1) { t = x1; x1 = x2; x2 = t; }
    if (y2 <= y1) { t = y1; y1 = y2; y2 = t; }
    if (x1 < 0) x1 = 0;
    if (x1 >= hres) x1 = hres - 1;
    if (x2 < 0) x2 = 0;
    if (x2 >= hres) x2 = hres - 1;
    if (y1 < 0) y1 = 0;
    if (y1 >= vres) y1 = vres - 1;
    if (y2 < 0) y2 = 0;
    if (y2 >= vres) y2 = vres - 1;

    // Convert RGB buffer to monochrome and set pixels
    int idx = 0;
    for (int y = y1; y <= y2; y++) {
        for (int x = x1; x <= x2; x++) {
            // Simple threshold for RGB to monochrome conversion
            int brightness = (p[idx] + p[idx + 1] + p[idx + 2]) / 3;
            ssd1309_set_pixel(x, y, brightness > 128 ? WHITE : BLACK);
            idx += 3;
        }
    }
}

void draw_bitmap_spi(int x1, int y1, int width, int height, int scale, int fc, int bc, unsigned char *bitmap) {
    int i, j, k, m, n;
    int vertCoord, horizCoord, XStart, XEnd, YEnd;

    if (x1 >= hres || y1 >= vres || x1 + width * scale < 0 || y1 + height * scale < 0) return;

    vertCoord = y1;
    if (y1 < 0) y1 = 0;
    XStart = x1;
    if (XStart < 0) XStart = 0;
    XEnd = x1 + (width * scale) - 1;
    if (XEnd >= hres) XEnd = hres - 1;
    YEnd = y1 + (height * scale) - 1;
    if (YEnd >= vres) YEnd = vres - 1;

    n = 0;
    for (i = 0; i < height; i++) {
        for (j = 0; j < scale; j++) {
            if (vertCoord++ < 0) continue;
            if (vertCoord > vres) return;
            horizCoord = x1;
            for (k = 0; k < width; k++) {
                for (m = 0; m < scale; m++) {
                    if (horizCoord++ < 0) continue;
                    if (horizCoord > hres) continue;
                    if ((bitmap[((i * width) + k) / 8] >> (((height * width) - ((i * width) + k) - 1) % 8)) & 1) {
                        ssd1309_set_pixel(horizCoord - 1, vertCoord - 1, fc);
                    } else {
                        if (bc != -1) {
                            ssd1309_set_pixel(horizCoord - 1, vertCoord - 1, bc);
                        }
                    }
                    n += 3;
                }
            }
        }
    }
}

void draw_rect_spi(int x1, int y1, int x2, int y2, int c) {
    ssd1309_draw_rect(x1, y1, x2, y2, c);
    ssd1309_display();
}

void draw_line_spi(int x1, int y1, int x2, int y2, int color) {
    ssd1309_draw_line(x1, y1, x2, y2, color);
    ssd1309_display();
}

void draw_battery_icon(int x0, int y0, int level) {
    ssd1309_draw_rect(x0, y0, x0+14, y0+6, WHITE);
    ssd1309_draw_rect(x0 + 1, y0 + 1, x0+12, y0+5, BLACK);
    ssd1309_draw_rect(x0 + 14, y0 + 2, x0+14+2, y0+2+2, WHITE);

    for (int i = 0; i <= 13; i++) {
        if (i < level) {
            ssd1309_draw_rect(x0 + 1 + i * 1, y0 + 1, x0 + 1 + i*1+1, y0+1+4, WHITE);
        }
    }
    ssd1309_display();
}

void lcd_print_char(int fc, int bc, char c, int orientation) {
    ssd1309_print_char(fc, bc, c, orientation);
}

void scroll_lcd_spi(int lines) {
    if (lines == 0) return;

    if (lines >= 0) {
        for (int i = 0; i < vres - lines; i++) {
            read_buffer_spi(0, i + lines, hres - 1, i + lines, scrollbuff);
            draw_buffer_spi(0, i, hres - 1, i, scrollbuff);
        }
        ssd1309_draw_rect(0, vres - lines, hres - 1, vres - 1, gui_bcolour);
    } else {
        lines = -lines;
        for (int i = vres - 1; i >= lines; i--) {
            read_buffer_spi(0, i - lines, hres - 1, i - lines, scrollbuff);
            draw_buffer_spi(0, i, hres - 1, i, scrollbuff);
        }
        ssd1309_draw_rect(0, 0, hres - 1, lines - 1, gui_bcolour);
    }
    ssd1309_display();
}

void display_put_c(char c) {
    if (c >= MainFont[2] && c < MainFont[2] + MainFont[3]) {
        if (current_x + gui_font_width > hres) {
            display_put_c('\r');
            display_put_c('\n');
        }
    }

    switch (c) {
        case '\b':
            current_x -= gui_font_width;
            if (current_x < 0) {
                current_y -= gui_font_height;
                if (current_y < 0) current_y = 0;
                current_x = (s_width - 1) * gui_font_width;
            }
            return;
        case '\r':
            current_x = 0;
            return;
        case '\n':
            current_x = 0;
            current_y += gui_font_height;
            if (current_y + gui_font_height >= vres) {
                scroll_lcd_spi(current_y + gui_font_height - vres);
                current_y -= (current_y + gui_font_height - vres);
            }
            return;
        case '\t':
            do {
                display_put_c(' ');
            } while ((current_x / gui_font_width) % 2);
            return;
    }
    lcd_print_char(gui_fcolour, gui_bcolour, c, ORIENT_NORMAL);
    ssd1309_display();
}

char lcd_put_char(char c, int flush) {
    lcd_putc(0, c);
    if (isprint(c)) lcd_char_pos++;
    if (c == '\r') {
        lcd_char_pos = 1;
    }
    return c;
}

void lcd_print_string(char *s) {
    while (*s) {
        if (s[1]) lcd_put_char(*s, 0);
        else lcd_put_char(*s, 1);
        s++;
    }
}

void lcd_print_string_color(char *s, int fg, int bg) {
    int old_fg = gui_fcolour;
    int old_bg = gui_bcolour;

    gui_fcolour = fg;
    gui_bcolour = bg;

    while (*s) {
        if (s[1]) lcd_put_char(*s, 0);
        else lcd_put_char(*s, 1);
        s++;
    }

    gui_fcolour = old_fg;
    gui_bcolour = old_bg;
}

void lcd_clear(void) {
    ssd1309_clear();
    ssd1309_display();
}

void lcd_putc(uint8_t devn, uint8_t c) {
    display_put_c(c);
}

int lcd_getc(uint8_t devn) {
    return read_i2c_kbd();
}

void lcd_sleeping(uint8_t devn) {
    // Put display to sleep
    ssd1309_command(SSD1309_DISPLAYOFF);
}

// SPI compatibility functions
unsigned char __not_in_flash_func(hw1_swap_spi)(unsigned char data_out) {
    unsigned char data_in = 0;
    spi_write_read_blocking(SSD1309_SPI_INST, &data_out, &data_in, 1);
    return data_in;
}

void hw_read_spi(unsigned char *buff, int cnt) {
    spi_read_blocking(SSD1309_SPI_INST, 0xff, buff, cnt);
}

void hw_send_spi(const unsigned char *buff, int cnt) {
    spi_write_blocking(SSD1309_SPI_INST, buff, cnt);
}

void pin_set_bit(int pin, unsigned int offset) {
    switch (offset) {
        case LATCLR:
            gpio_set_pulls(pin, false, false);
            gpio_pull_down(pin);
            gpio_put(pin, 0);
            return;
        case LATSET:
            gpio_set_pulls(pin, false, false);
            gpio_pull_up(pin);
            gpio_put(pin, 1);
            return;
        case LATINV:
            gpio_xor_mask(1 << pin);
            return;
        case TRISSET:
            gpio_set_dir(pin, GPIO_IN);
            sleep_us(2);
            return;
        case TRISCLR:
            gpio_set_dir(pin, GPIO_OUT);
            gpio_set_drive_strength(pin, GPIO_DRIVE_STRENGTH_12MA);
            sleep_us(2);
            return;
        case CNPUSET:
            gpio_set_pulls(pin, true, false);
            return;
        case CNPDSET:
            gpio_set_pulls(pin, false, true);
            return;
        case CNPUCLR:
        case CNPDCLR:
            gpio_set_pulls(pin, false, false);
            return;
        case ODCCLR:
            gpio_set_dir(pin, GPIO_OUT);
            gpio_put(pin, 0);
            sleep_us(2);
            return;
        case ODCSET:
            gpio_set_pulls(pin, true, false);
            gpio_set_dir(pin, GPIO_IN);
            sleep_us(2);
            return;
        case ANSELCLR:
            gpio_set_function(pin, GPIO_FUNC_SIO);
            gpio_set_dir(pin, GPIO_IN);
            return;
        default:
            break;
    }
}

void reset_controller(void) {
    pin_set_bit(OLED_RST, LATSET);
    sleep_us(10000);
    pin_set_bit(OLED_RST, LATCLR);
    sleep_us(10000);
    pin_set_bit(OLED_RST, LATSET);
    sleep_us(200000);
}

void lcd_spi_raise_cs(void) {
    gpio_put(OLED_CS, 1);
}

void lcd_set_cursor(int x, int y) {
    current_x = x;
    current_y = y;
}

void lcd_spi_lower_cs(void) {
    gpio_put(OLED_CS, 0);
}

void spi_write_data(unsigned char data) {
    ssd1309_data(data);
}

void spi_write_data24(uint32_t data) {
    // For monochrome OLED, convert 24-bit to 1-bit
    uint8_t mono = ((data >> 16) + ((data >> 8) & 0xFF) + (data & 0xFF)) / 3 > 128 ? 0xFF : 0x00;
    ssd1309_data(mono);
}

void spi_write_command(unsigned char data) {
    ssd1309_command(data);
}

void spi_write_cd(unsigned char command, int data, ...) {
    int i;
    va_list ap;
    va_start(ap, data);
    spi_write_command(command);
    for (i = 0; i < data; i++) spi_write_data((char) va_arg(ap, int));
    va_end(ap);
}

void lcd_spi_init(void) {
    ssd1309_spi_init();
}

void lcd_spi_deinit(void){
    spi_deinit(SSD1309_SPI_INST);
    gpio_deinit(OLED_RST);
    gpio_deinit(OLED_DC);
    gpio_deinit(OLED_CS);
    gpio_deinit(OLED_SCLK);
    gpio_deinit(OLED_MOSI);
}

void lcd_init(void) {
    lcd_spi_init();
    ssd1309_init();
    set_font();
    gui_fcolour = WHITE;
    gui_bcolour = BLACK;
}
