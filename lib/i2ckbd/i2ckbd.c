#include <pico/stdio.h>
#include <pico/time.h>
#include <stddef.h>
#include <hardware/i2c.h>
#include <hardware/gpio.h>
#include "i2ckbd.h"

typedef struct {
    uint16_t pot_adc_values[8]; // raw adc values
    uint16_t pot_positions[4]; // absolute position 0..1023
    uint8_t pot_states[4]; // BIT0: fwd, BIT1: bwd, BIT2: fast
    // 16-bit
    uint16_t d_btns; // BIT0-15: D1-D16
    uint16_t d_btns_long_press; // BIT0-15: D1-D16
    // function buttons are (0: F1, 1: F2, 2: POT1 (left), 3: POT2, 4: POT3, 5: POT4 (right))
    // 6-bit
    uint8_t f_btns;
    uint8_t f_btns_long_press;
    // mcl buttons are (0: MCL_LEFT, 1: MCL_DOWN, 2: MCL_RIGHT, 3: MCL_UP, 4: MCL_A, 5: MCL_B, 6: MCL_X, 7: MCL_Y, 8: MCL_P, 9: MCL_R, 10: MCL_S1, 11: MCL_S2)
    // 12-bit
    uint16_t mcl_btns;
    uint16_t mcl_btns_long_press;
    int16_t accelerometer[3];
    uint32_t systicks; // timestamp
} ui_data_t;

static uint8_t i2c_inited = 0;
static absolute_time_t last_poll_time;
static int32_t pot_accumulator = 0;
static uint16_t latest_mcl_btns = 0;
static uint16_t latest_mcl_btns_long = 0;

typedef enum {
    KEY_NONE = -1,
    KEY_ENTER = 0x0A,
    KEY_PG_UP = 0xB3,
    KEY_PG_DOWN = 0xB4,
    KEY_POT_DOWN = 0xB5,
    KEY_POT_UP = 0xB6,
    KEY_LEFT = 0x81,
    KEY_RIGHT = 0x82,
    KEY_DOWN = 0x83,
    KEY_UP = 0x84,
    KEY_FIRE_A = 0x85,
    KEY_FIRE_B = 0x86,
    KEY_LEFT_FAST = 0x91,
    KEY_RIGHT_FAST = 0x92,
    KEY_DOWN_FAST = 0x93,
    KEY_UP_FAST = 0x94
} keycode_t;

static keycode_t queued_keys[8];
static uint8_t queue_head = 0;
static uint8_t queue_tail = 0;

static inline bool queue_is_empty(void) {
    return queue_head == queue_tail;
}

static inline bool queue_is_full(void) {
    return ((queue_head + 1) & 0x7) == queue_tail;
}

static void queue_push(keycode_t key) {
    if (key <= KEY_NONE) return;
    if (queue_is_full()) {
        queue_tail = (queue_tail + 1) & 0x7;
    }
    queued_keys[queue_head] = key;
    queue_head = (queue_head + 1) & 0x7;
}

static keycode_t queue_pop(void) {
    if (queue_is_empty()) return KEY_NONE;
    keycode_t key = queued_keys[queue_tail];
    queue_tail = (queue_tail + 1) & 0x7;
    return key;
}

static void enqueue_direction_keys(const ui_data_t *buff) {
    const uint16_t btns = buff->mcl_btns;
    const uint16_t long_btns = buff->mcl_btns_long_press;
    static uint16_t last_btns = 0;
    static uint16_t last_long = 0;
    static uint8_t repeat_ticks[4] = {0};
    const uint8_t repeat_threshold = 4;
    const struct {
        uint8_t bit;
        keycode_t key;
        keycode_t fast_key;
    } mapping[] = {
        {0, KEY_LEFT, KEY_LEFT_FAST},
        {1, KEY_DOWN, KEY_DOWN_FAST},
        {2, KEY_RIGHT, KEY_RIGHT_FAST},
        {3, KEY_UP, KEY_UP_FAST},
        {4, KEY_FIRE_A, KEY_NONE},
        {5, KEY_FIRE_B, KEY_NONE}
    };

    for (size_t i = 0; i < sizeof(mapping)/sizeof(mapping[0]); ++i) {
        uint8_t bit = mapping[i].bit;
        bool pressed = (btns & (1u << bit)) != 0;
        bool was_pressed = (last_btns & (1u << bit)) != 0;
        if (pressed && !was_pressed) {
            queue_push(mapping[i].key);
        }
        bool long_pressed = (long_btns & (1u << bit)) != 0;
        bool was_long = (last_long & (1u << bit)) != 0;
        if (long_pressed && !was_long) {
            queue_push(mapping[i].key);
        }
    }

    for (size_t idx = 0; idx < 4; ++idx) {
        bool is_long = (long_btns & (1u << idx)) != 0;
        if (is_long) {
            if (++repeat_ticks[idx] >= repeat_threshold) {
                queue_push(mapping[idx].fast_key);
                repeat_ticks[idx] = 0;
            }
        } else {
            repeat_ticks[idx] = 0;
        }
    }

    last_btns = btns;
    last_long = long_btns;
}

static void enqueue_function_keys(const ui_data_t *buff) {
    const uint8_t long_fn = buff->f_btns_long_press;
    static uint8_t last_long_fn = 0;
    const struct {
        uint8_t bit;
        keycode_t key;
    } fn_mapping[] = {
        {2, KEY_ENTER}
    };
    for (size_t i = 0; i < sizeof(fn_mapping)/sizeof(fn_mapping[0]); ++i) {
        uint8_t bit = fn_mapping[i].bit;
        bool pressed = (long_fn & (1u << bit)) != 0;
        bool was_pressed = (last_long_fn & (1u << bit)) != 0;
        if (pressed && !was_pressed) {
            queue_push(fn_mapping[i].key);
        }
    }
    last_long_fn = long_fn;
}

static void enqueue_pager_keys(const ui_data_t *buff) {
    const uint16_t btns = buff->mcl_btns;
    static uint16_t last_btns = 0;
    struct {
        uint8_t bit;
        keycode_t key;
    } map[] = {
        {3, KEY_PG_UP},
        {1, KEY_PG_DOWN}
    };
    for (size_t i = 0; i < sizeof(map)/sizeof(map[0]); ++i) {
        bool pressed = (btns & (1u << map[i].bit)) != 0;
        bool was_pressed = (last_btns & (1u << map[i].bit)) != 0;
        if (pressed && !was_pressed) {
            queue_push(map[i].key);
        }
    }
    last_btns = btns;
}

static void enqueue_pot_keys(const ui_data_t *buff) {
    static uint16_t last_position = 0;
    static bool initialized = false;
    if (!initialized) {
        last_position = buff->pot_positions[0];
        initialized = true;
        return;
    }
    int16_t diff = (int16_t)(buff->pot_positions[0] - last_position);
    if (diff > 512) diff -= 1024;
    else if (diff < -512) diff += 1024;

    pot_accumulator += diff;
    const int16_t threshold = 25;
    while (pot_accumulator >= threshold) {
        queue_push(KEY_POT_UP);
        pot_accumulator -= threshold;
    }
    while (pot_accumulator <= -threshold) {
        queue_push(KEY_POT_DOWN);
        pot_accumulator += threshold;
    }

    last_position = buff->pot_positions[0];
}

static void poll_i2c_if_needed(void) {
    i2c_kbd_poll();
}

void i2c_kbd_poll(void) {
    if (!i2c_inited) return;
    absolute_time_t now = get_absolute_time();
    if (absolute_time_diff_us(last_poll_time, now) < 2500) {
        return;
    }
    ui_data_t buf;
    int retval = i2c_read_timeout_us(I2C_KBD_MOD, I2C_KBD_ADDR, (unsigned char *) &buf, sizeof(ui_data_t), false, 500000);
    if (retval == PICO_ERROR_GENERIC || retval == PICO_ERROR_TIMEOUT) {
        return;
    }
    last_poll_time = now;

    latest_mcl_btns = buf.mcl_btns;
    latest_mcl_btns_long = buf.mcl_btns_long_press;
    enqueue_direction_keys(&buf);
    enqueue_function_keys(&buf);
    enqueue_pager_keys(&buf);
    enqueue_pot_keys(&buf);
}

static keycode_t get_key_from_queue(void) {
    i2c_kbd_poll();
    return queue_pop();
}

static void reset_stm32(void) {
    gpio_init(STM32RESET_PIN);
    gpio_set_dir(STM32RESET_PIN, GPIO_OUT);
    gpio_put(STM32RESET_PIN, 0);
    sleep_ms(10);
    gpio_put(STM32RESET_PIN, 1);
    sleep_ms(50);
}

void init_i2c_kbd(void) {
    reset_stm32();
    gpio_set_function(I2C_KBD_SCL, GPIO_FUNC_I2C);
    gpio_set_function(I2C_KBD_SDA, GPIO_FUNC_I2C);
    i2c_init(I2C_KBD_MOD, I2C_KBD_SPEED);
    gpio_pull_up(I2C_KBD_SCL);
    gpio_pull_up(I2C_KBD_SDA);
    i2c_inited = 1;
}

void deinit_i2c_kbd(void) {
    if (!i2c_inited) return;
    i2c_deinit(I2C_KBD_MOD);
    gpio_deinit(I2C_KBD_SCL);
    gpio_deinit(I2C_KBD_SDA);
    reset_stm32();
    gpio_deinit(STM32RESET_PIN);
    i2c_inited = 0;
}

int read_i2c_kbd() {
    keycode_t key = get_key_from_queue();
    return key == KEY_NONE ? -1 : key;
}

void i2c_kbd_get_mcl_state(uint16_t *buttons, uint16_t *long_buttons) {
    i2c_kbd_poll();
    if (buttons) {
        *buttons = latest_mcl_btns;
    }
    if (long_buttons) {
        *long_buttons = latest_mcl_btns_long;
    }
}

int read_battery() {
    return -1;
}
