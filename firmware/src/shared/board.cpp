#include "board.h"

static Arduino_ESP32RGBPanel *bus = new Arduino_ESP32RGBPanel(
    40 /* DE */, 41 /* VSYNC */, 39 /* HSYNC */, 42 /* PCLK */,
    45 /* R0 */, 48 /* R1 */, 47 /* R2 */, 21 /* R3 */, 14 /* R4 */,
    5 /* G0 */, 6 /* G1 */, 7 /* G2 */, 15 /* G3 */, 16 /* G4 */, 4 /* G5 */,
    8 /* B0 */, 3 /* B1 */, 46 /* B2 */, 9 /* B3 */, 1 /* B4 */,
    0 /* hsync_polarity */, 8 /* hsync_front_porch */, 4 /* hsync_pulse_width */, 8 /* hsync_back_porch */,
    0 /* vsync_polarity */, 8 /* vsync_front_porch */, 4 /* vsync_pulse_width */, 8 /* vsync_back_porch */,
    1 /* pclk_active_neg */, PANEL_PCLK_HZ /* prefer_speed */, false /* big endian */, 0 /* de idle high */, 0 /* pclk idle high */,
    PANEL_BOUNCE_PX /* bounce buffer px */);

Arduino_RGB_Display *gfx = new Arduino_RGB_Display(SCREEN_W, SCREEN_H, bus, 0 /* rotation */, true /* auto_flush */);

void board_init() {
  pinMode(PIN_BL, OUTPUT);
  digitalWrite(PIN_BL, LOW);   // keep dark until the first frame is drawn
  if (!gfx->begin()) {
    Serial.println("gfx->begin() failed");
  }
  gfx->fillScreen(0x0000);
  digitalWrite(PIN_BL, HIGH);
}

// Backlight dimming: PWM on PIN_BL. 20 kHz gave a black panel at 90 % on this board (2026-11-05), 1 kHz works (CONFIRMED
// 2026-11-05). Debug: GET /bl?pct=50&hz=1000 on the screen. 100 % = plain HIGH.
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
