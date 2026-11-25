// Backlight dimming, both boards: PWM on the board's PIN_BL at 1 kHz (20 kHz gives a black panel at 90 % on the S3
// board). 100 % = the pin plain HIGH. Debug: GET /bl?pct=50&hz=1000 on the screen (board_backlight_hz).
#include <Arduino.h>
#include "board_pins.h"
#include "board.h"
#include "board_core.h"

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
    pin_output(PIN_BL, 1);
    return;
  }
  if (!g_bl_pwm) {
    if (!ledcAttach(PIN_BL, g_bl_hz, 10)) {
      printf("backlight: PWM at %u Hz failed, staying fully on\n", (unsigned)g_bl_hz);
      pin_output(PIN_BL, 1);
      return;
    }
    g_bl_pwm = true;
  }
  ledcWrite(PIN_BL, (uint32_t)(pct * 1023 / 100));
  printf("backlight: %d %% at %u Hz\n", pct, (unsigned)g_bl_hz);
}
