#ifndef I2C_KEYBOARD_H
#define I2C_KEYBOARD_H
#include <pico/stdlib.h>
#include <pico/platform.h>
#include <hardware/gpio.h>
#include <hardware/i2c.h>

#define I2C_KBD_MOD i2c1
#define I2C_KBD_SDA 38
#define I2C_KBD_SCL 39

#define I2C_KBD_SPEED 400000 // 400kHz

#define I2C_KBD_ADDR 0x42

#define STM32RESET_PIN 40

void init_i2c_kbd();
void deinit_i2c_kbd();
int read_i2c_kbd();
int read_battery();
void i2c_kbd_poll();
void i2c_kbd_get_mcl_state(uint16_t *buttons, uint16_t *long_buttons);

#define bitRead(value, bit) (((value) >> (bit)) & 0x01)
#define bitClear(value, bit) ((value) &= ~(1 << (bit)))
#define bitSet(value, bit) ((value) |= (1 << (bit)))



#endif
