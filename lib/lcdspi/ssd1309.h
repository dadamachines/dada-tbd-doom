#ifndef SSD1309_H
#define SSD1309_H

#include "pico/stdlib.h"
#include "pico/multicore.h"
#include <hardware/spi.h>
#include <hardware/gpio.h>

// SPI pin definitions for SSD1309
#define OLED_MOSI 15
#define OLED_SCLK 14
#define OLED_DC   12
#define OLED_CS   13
#define OLED_RST  16

// SSD1309 display dimensions
#define SSD1309_WIDTH  128
#define SSD1309_HEIGHT 64

// Compatibility definitions for lcdspi interface
#define LCD_WIDTH  SSD1309_WIDTH
#define LCD_HEIGHT SSD1309_HEIGHT

// SPI settings - use same SPI instance and speed as original
#define SSD1309_SPI_SPEED   8000000  // Match LCD_SPI_SPEED
#define SSD1309_SPI_INST    spi1      // Match Pico_LCD_SPI_MOD

// Pin mapping for compatibility (these map to the OLED pins)
#define Pico_LCD_SCK  OLED_SCLK
#define Pico_LCD_TX   OLED_MOSI
#define Pico_LCD_RX   OLED_MOSI  // Not used in OLED, map to MOSI
#define Pico_LCD_CS   OLED_CS
#define Pico_LCD_DC   OLED_DC
#define Pico_LCD_RST  OLED_RST
#define Pico_LCD_SPI_MOD SSD1309_SPI_INST
#define LCD_SPI_SPEED SSD1309_SPI_SPEED

// Color definitions - 1-bit display (monochrome)
#define RGB(red, green, blue) ((red > 128 || green > 128 || blue > 128) ? 1 : 0)
#define WHITE               1
#define YELLOW              1
#define LILAC               1
#define BROWN               1
#define FUCHSIA             1
#define RUST                1
#define MAGENTA             1
#define RED                 1
#define CYAN                1
#define GREEN               1
#define CERULEAN            1
#define MIDGREEN            1
#define COBALT              1
#define MYRTLE              1
#define BLUE                1
#define BLACK               0
#define GRAY                1
#define LITEGRAY            1
#define ORANGE              1
#define PINK                1
#define GOLD                1
#define SALMON              1
#define BEIGE               1

// Font orientation
#define ORIENT_NORMAL 0

// Hardware register definitions for compatibility
#define PORTCLR             1
#define PORTSET             2
#define PORTINV             3
#define LAT                 4
#define LATCLR              5
#define LATSET              6
#define LATINV              7
#define ODC                 8
#define ODCCLR              9
#define ODCSET              10
#define CNPU                12
#define CNPUCLR             13
#define CNPUSET             14
#define CNPUINV             15
#define CNPD                16
#define CNPDCLR             17
#define CNPDSET             18
#define ANSELCLR            -7
#define ANSELSET            -6
#define ANSELINV            -5
#define TRIS                -4
#define TRISCLR             -3
#define TRISSET             -2

// SSD1309 specific commands
#define SSD1309_SETCONTRAST         0x81
#define SSD1309_DISPLAYALLON_RESUME 0xA4
#define SSD1309_DISPLAYALLON        0xA5
#define SSD1309_NORMALDISPLAY       0xA6
#define SSD1309_INVERTDISPLAY       0xA7
#define SSD1309_DISPLAYOFF          0xAE
#define SSD1309_DISPLAYON           0xAF
#define SSD1309_SETDISPLAYOFFSET    0xD3
#define SSD1309_SETCOMPINS          0xDA
#define SSD1309_SETVCOMDETECT       0xDB
#define SSD1309_SETDISPLAYCLOCKDIV  0xD5
#define SSD1309_SETPRECHARGE        0xD9
#define SSD1309_SETMULTIPLEX        0xA8
#define SSD1309_SETLOWCOLUMN        0x00
#define SSD1309_SETHIGHCOLUMN       0x10
#define SSD1309_SETSTARTLINE        0x40
#define SSD1309_MEMORYMODE          0x20
#define SSD1309_COLUMNADDR          0x21
#define SSD1309_PAGEADDR            0x22
#define SSD1309_COMSCANINC          0xC0
#define SSD1309_COMSCANDEC          0xC8
#define SSD1309_SEGREMAP            0xA0
#define SSD1309_CHARGEPUMP          0x8D
#define SSD1309_COMMANDLOCK         0xFD

// Native SSD1309 function declarations
bool ssd1309_init(void);
void ssd1309_clear(void);
void ssd1309_display(void);
void ssd1309_set_pixel(int x, int y, int color);
void ssd1309_draw_rect(int x1, int y1, int x2, int y2, int color);
void ssd1309_draw_line(int x1, int y1, int x2, int y2, int color);
void ssd1309_set_cursor(int x, int y);
void ssd1309_print_char(int fc, int bc, char c, int orientation);
void ssd1309_print_string(char *s);
void ssd1309_print_string_color(char *s, int fg, int bg);
void ssd1309_command(uint8_t cmd);
bool ssd1309_command_list(const uint8_t *commands, size_t count);
void ssd1309_data(uint8_t data);
void ssd1309_spi_init(void);

// Complete lcdspi API compatibility functions
void lcd_init(void);
void lcd_clear(void);
void lcd_set_cursor(int x, int y);
void lcd_putc(uint8_t devn, uint8_t c);
int lcd_getc(uint8_t devn);
void lcd_print_string(char *s);
void lcd_print_string_color(char *s, int fg, int bg);
char lcd_put_char(char c, int flush);
void lcd_sleeping(uint8_t devn);

// Drawing functions
void draw_rect_spi(int x1, int y1, int x2, int y2, int c);
void draw_line_spi(int x1, int y1, int x2, int y2, int color);
void draw_bitmap_spi(int x1, int y1, int width, int height, int scale, int fc, int bc, unsigned char *bitmap);
void draw_buffer_spi(int x1, int y1, int x2, int y2, unsigned char *p);
void draw_battery_icon(int x0, int y0, int level);
void define_region_spi(int xstart, int ystart, int xend, int yend, int rw);
void read_buffer_spi(int x1, int y1, int x2, int y2, unsigned char *p);

// SPI low-level functions
void lcd_spi_init(void);
void lcd_spi_deinit(void);
void lcd_spi_raise_cs(void);
void lcd_spi_lower_cs(void);
void spi_write_data(unsigned char data);
void spi_write_command(unsigned char data);
void spi_write_cd(unsigned char command, int data, ...);
void spi_write_data24(uint32_t data);
void hw_read_spi(unsigned char *buff, int cnt);
void hw_send_spi(const unsigned char *buff, int cnt);
unsigned char hw1_swap_spi(unsigned char data_out);
void spi_write_fast(spi_inst_t *spi, const uint8_t *src, size_t len);
void spi_finish(spi_inst_t *spi);

// Utility functions
void reset_controller(void);
void pin_set_bit(int pin, unsigned int offset);
void scroll_lcd_spi(int lines);
void set_font(void);

#endif // SSD1309_H
