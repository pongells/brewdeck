// Backlight dimming, both boards: PWM on the board's PIN_BL at 1 kHz (20 kHz gives a black panel at 90 % on the S3
// board). 100 % = the pin plain HIGH. Debug: GET /bl?pct=50&hz=1000 on the screen (board_backlight_hz).
#include <driver/ledc.h>
#include "board_pins.h"
#include "board.h"
#include "board_core.h"

// ESP-IDF's LEDC driver: timer 0 + channel 0 (nothing else uses LEDC), 10 bits
static const ledc_mode_t BL_MODE = LEDC_LOW_SPEED_MODE;
static const ledc_channel_t BL_CH = LEDC_CHANNEL_0;
static uint32_t g_bl_hz = 1000;
static bool g_bl_pwm = false;
static bool pwm_start(void) {
  ledc_timer_config_t t = {};
  t.speed_mode = BL_MODE;
  t.duty_resolution = LEDC_TIMER_10_BIT;
  t.timer_num = LEDC_TIMER_0;
  t.freq_hz = g_bl_hz;
  t.clk_cfg = LEDC_AUTO_CLK;
  if (ledc_timer_config(&t) != ESP_OK) return false;
  ledc_channel_config_t c = {};
  c.gpio_num = PIN_BL;
  c.speed_mode = BL_MODE;
  c.channel = BL_CH;
  c.timer_sel = LEDC_TIMER_0;
  c.duty = 1023;                                    // fully on until the first duty is set: no dark flash
  return ledc_channel_config(&c) == ESP_OK;
}
static void pwm_stop(void) {                        // the pin stays high: plain output next (pin_output)
  ledc_stop(BL_MODE, BL_CH, 1);
  g_bl_pwm = false;
}
void board_backlight_hz(uint32_t hz) {
  if (hz < 100 || hz > 40000) return;
  g_bl_hz = hz;
  if (g_bl_pwm) pwm_stop();
}
void board_backlight(int pct) {
  if (pct < 0) pct = 0;
  if (pct >= 100) {
    if (g_bl_pwm) pwm_stop();
    pin_output(PIN_BL, 1);
    return;
  }
  if (!g_bl_pwm) {
    if (!pwm_start()) {
      printf("backlight: PWM at %u Hz failed, staying fully on\n", (unsigned)g_bl_hz);
      pin_output(PIN_BL, 1);
      return;
    }
    g_bl_pwm = true;
  }
  ledc_set_duty(BL_MODE, BL_CH, (uint32_t)(pct * 1023 / 100));
  ledc_update_duty(BL_MODE, BL_CH);
  printf("backlight: %d %% at %u Hz\n", pct, (unsigned)g_bl_hz);
}
