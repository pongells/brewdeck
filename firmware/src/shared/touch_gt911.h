#pragma once
#include <stdint.h>

// Minimal GT911 capacitive touch driver (single touch point).
bool    touch_begin();                          // reset + probe; false if no controller found
uint8_t touch_address();                        // I2C address found (0x5D or 0x14), 0 if none
bool    touch_pressed(int16_t *x, int16_t *y);  // true while a finger is down; x,y in screen pixels
bool    touch_raw(int16_t *rx, int16_t *ry);    // same, but raw controller coordinates (for the test)
void    touch_map(int16_t rx, int16_t ry, int16_t *x, int16_t *y);  // apply swap/flip settings
void touch_cfg(int *xmax, int *ymax);   // coordinate range the chip reports (for scaling)
