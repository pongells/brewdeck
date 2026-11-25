// Plain output pins on ESP-IDF's GPIO driver (backlight on / off, reset lines).
#pragma once
#include <driver/gpio.h>

// makes pin an output at level (set first, so the pin never shows the other level)
static inline void pin_output(int pin, int level) {
  gpio_set_level((gpio_num_t)pin, level);
  gpio_config_t c = {};
  c.pin_bit_mask = 1ULL << pin;
  c.mode = GPIO_MODE_OUTPUT;
  gpio_config(&c);
  gpio_set_level((gpio_num_t)pin, level);
}
static inline void pin_set(int pin, int level) { gpio_set_level((gpio_num_t)pin, level); }
