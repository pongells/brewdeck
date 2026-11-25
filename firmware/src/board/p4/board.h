#pragma once
// Guition JC4880P443 (ESP32-P4 rev 3): 480x800 portrait MIPI-DSI panel (ST7701) shown as the UI's 800x480
// landscape, GT911 touch, Wi-Fi through the on-board ESP32-C6. Panel settings from Arduino_GFX's JC4880P433 example;
// raw touch coordinates are portrait panel pixels.
#include <Arduino.h>
#include <Arduino_GFX_Library.h>

#define SCREEN_W 800   // what the UI draws
#define SCREEN_H 480

// Rotation (Arduino_GFX): 1 = the panel's right edge (held in portrait) is the picture's top, 3 = its left edge.
// 3: the board's SD card slot is at the bottom of the picture.
#ifndef PANEL_ROT
#define PANEL_ROT 3
#endif
#define PANEL_W 480
#define PANEL_H 800
#define PIN_BL        23
#define PIN_PANEL_RST 5
#define PIN_TOUCH_SDA 7
#define PIN_TOUCH_SCL 8
#define PIN_TOUCH_RST (-1)   // not on a GPIO we know: no reset pulse

// Touch orientation: portrait raw (x 0-479, y 0-799) -> landscape, matching PANEL_ROT
#define TOUCH_SWAP_XY 1
#if PANEL_ROT == 1
#define TOUCH_FLIP_X 0
#define TOUCH_FLIP_Y 1
#else
#define TOUCH_FLIP_X 1
#define TOUCH_FLIP_Y 0
#endif

extern Arduino_DSI_Display *gfx;
void board_init();
void board_backlight(int level);        // 0 = off, 20-100 = dimmest useful .. fully on (backlight.cpp)
// the PWM duty at level 20, in 1/1000: 1 % is dim but still readable on this panel; 5 % still looked bright (seen on the panel)
#define BL_MIN_PERMILLE 10
