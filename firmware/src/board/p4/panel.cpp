// Guition JC4880P443 panel: Arduino_GFX's DSI driver (ST7701) and the backlight. Used by board_p4.cpp.
#include "board.h"

// panel timing from Arduino_GFX's JC4880P433 example (seen right on this board, 2026-11-18)
static Arduino_ESP32DSIPanel *bus = new Arduino_ESP32DSIPanel(
    12 /* hsync_pulse_width */, 42 /* hsync_back_porch */, 42 /* hsync_front_porch */,
    2 /* vsync_pulse_width */, 8 /* vsync_back_porch */, 166 /* vsync_front_porch */,
    34000000 /* prefer_speed */);
Arduino_DSI_Display *gfx = new Arduino_DSI_Display(PANEL_W, PANEL_H, bus, PANEL_ROT, true /* IPS */, PIN_PANEL_RST,
    st7701_dsi_init_operations, sizeof(st7701_dsi_init_operations) / sizeof(lcd_init_cmd_t));

void board_init() {
  pinMode(PIN_BL, OUTPUT);
  digitalWrite(PIN_BL, LOW);   // keep dark until the first frame is drawn
  if (!gfx->begin()) Serial.println("gfx->begin() failed");
  gfx->setRotation(PANEL_ROT);
  gfx->fillScreen(0x0000);
  digitalWrite(PIN_BL, HIGH);
}

// Backlight dimming: PWM on PIN_BL, 1 kHz as on the S3 (not yet checked against flicker on this panel). 100 % = HIGH.
static uint32_t g_bl_hz = 1000;
static bool g_bl_pwm = false;
void board_backlight_hz(uint32_t hz) {
  if (hz < 100 || hz > 40000) return;
  g_bl_hz = hz;
  if (g_bl_pwm) { ledcDetach(PIN_BL); g_bl_pwm = false; }
}
void board_backlight(int pct) {
  if (pct < 0) pct = 0;
  if (pct >= 100) {
    if (g_bl_pwm) { ledcDetach(PIN_BL); g_bl_pwm = false; }
    pinMode(PIN_BL, OUTPUT);
    digitalWrite(PIN_BL, HIGH);
    return;
  }
  if (!g_bl_pwm) {
    if (!ledcAttach(PIN_BL, g_bl_hz, 10)) {
      Serial.printf("backlight: PWM at %u Hz failed, staying fully on\n", (unsigned)g_bl_hz);
      pinMode(PIN_BL, OUTPUT);
      digitalWrite(PIN_BL, HIGH);
      return;
    }
    g_bl_pwm = true;
  }
  ledcWrite(PIN_BL, (uint32_t)(pct * 1023 / 100));
  Serial.printf("backlight: %d %% at %u Hz\n", pct, (unsigned)g_bl_hz);
}

