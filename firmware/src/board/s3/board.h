#pragma once
// Sunton ESP32-S3 4.3" 800x480 RGB panel (ESP32-8048S043-style; also the Gaggiuino Gen 3 screen) with GT911 touch.
// Pins from public documentation, confirmed by the first-light test.
#include <stdint.h>

#define SCREEN_W 800   // what the UI draws
#define SCREEN_H 480

// Pixel clock. Default 12 MHz; firmware/CMakeLists.txt sets 16 MHz (stable there with the bounce buffer and its sdkconfig).
#ifndef PANEL_PCLK_HZ
#define PANEL_PCLK_HZ 12000000
#endif
// Bounce buffer (pixels): lets the LCD peripheral read from fast internal RAM instead of PSRAM (firmware/CMakeLists.txt
// sets 8000; 0 = off).
#ifndef PANEL_BOUNCE_PX
#define PANEL_BOUNCE_PX 0
#endif
#define PIN_BL        2
#define PIN_TOUCH_SDA 19
#define PIN_TOUCH_SCL 20
#define PIN_TOUCH_RST 38

// Touch orientation. Adjust these if the touch test shows the axes wrong.
#define TOUCH_SWAP_XY 0   // 1 = swap raw X and Y first
#define TOUCH_FLIP_X  0   // 1 = mirror horizontally (after swap)
#define TOUCH_FLIP_Y  0   // 1 = mirror vertically (after swap)

void board_init();                      // panel up, black, backlight on (panel.cpp)
uint16_t *board_fb(void);               // the panel's frame buffer in PSRAM (SCREEN_W x SCREEN_H RGB565), NULL if none
void board_backlight(int level);        // 0 = off, 20-100 = dimmest useful .. fully on (backlight.cpp)
// the PWM duty at level 20, in 1/1000: 20 % is already very dim on this panel (seen on the panel)
#define BL_MIN_PERMILLE 200
