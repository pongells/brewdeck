#pragma once
// Sunton ESP32-S3 4.3" 800x480 RGB panel (ESP32-8048S043-style; also the Gaggiuino Gen 3 screen) with GT911 touch.
// Pins from public documentation, confirmed by the first-light test.
#include <Arduino.h>
#include <Arduino_GFX_Library.h>

#define SCREEN_W 800   // what the UI draws
#define SCREEN_H 480

// Pixel clock. Default 12 MHz; platformio.ini sets 16 MHz (stable there with the bounce buffer and its sdkconfig).
#ifndef PANEL_PCLK_HZ
#define PANEL_PCLK_HZ 12000000
#endif
// Bounce buffer (pixels): lets the LCD peripheral read from fast internal RAM instead of PSRAM (platformio.ini sets
// 8000; 0 = off).
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

extern Arduino_RGB_Display *gfx;
void board_init();
void board_backlight(int pct);          // 0-100 %; PWM on PIN_BL (100 = fully on, as after board_init)
void board_backlight_hz(uint32_t hz);   // PWM frequency for dimming (debug, see panel.cpp)
