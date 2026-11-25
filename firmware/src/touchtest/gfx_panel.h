// The touch test's panel (Arduino_GFX, plain Arduino build): gfx draws on the S3 board's panel.
#pragma once
#include <Arduino.h>
#include <Arduino_GFX_Library.h>
#include "board.h"
extern Arduino_RGB_Display *gfx;
void gfx_panel_init();
