// Guition JC4880P443 panel: Arduino_GFX's DSI driver (ST7701) and the backlight. Used by board_p4.cpp.
#include "board.h"

// panel timing from Arduino_GFX's JC4880P433 example
static Arduino_ESP32DSIPanel *bus = new Arduino_ESP32DSIPanel(
    12 /* hsync_pulse_width */, 42 /* hsync_back_porch */, 42 /* hsync_front_porch */,
    2 /* vsync_pulse_width */, 8 /* vsync_back_porch */, 166 /* vsync_front_porch */,
    34000000 /* prefer_speed */);
Arduino_DSI_Display *gfx = new Arduino_DSI_Display(PANEL_W, PANEL_H, bus, PANEL_ROT, true /* auto_flush: cache write-back */, PIN_PANEL_RST,
    st7701_dsi_init_operations, sizeof(st7701_dsi_init_operations) / sizeof(lcd_init_cmd_t));

void board_init() {
  pinMode(PIN_BL, OUTPUT);
  digitalWrite(PIN_BL, LOW);   // keep dark until the first frame is drawn
  if (!gfx->begin()) Serial.println("gfx->begin() failed");
  gfx->setRotation(PANEL_ROT);
  gfx->fillScreen(0x0000);
  delay(50);                   // ~3 frames of the black fill first: lit at once, the panel showed its blue for a frame
  digitalWrite(PIN_BL, HIGH);
}

