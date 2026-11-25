// Sunton ESP32-S3 panel on ESP-IDF's RGB LCD driver (esp_lcd): one frame buffer in PSRAM that the LCD peripheral
// scans out on its own, through two bounce buffers in internal RAM (the DMA reads those, the driver's interrupt refills
// them from PSRAM), so the picture holds while Wi-Fi and flash are busy (with the settings in sdkconfig.brewdeck.esp32s3).
// Timing and pins as Arduino_GFX had them (src/touchtest/gfx_panel.cpp still uses that).
#include <string.h>
#include <esp_lcd_panel_rgb.h>
#include <esp_lcd_panel_ops.h>
#include "esp32s3/rom/cache.h"
#include "board.h"
#include "board_pins.h"
#include "../../core/systime.h"

static esp_lcd_panel_handle_t g_panel;
static uint16_t *g_fb;
uint16_t *board_fb(void) { return g_fb; }

void board_init() {
  pin_output(PIN_BL, 0);   // keep dark until the first frame is drawn
  esp_lcd_rgb_panel_config_t c;
  memset(&c, 0, sizeof c);
  c.clk_src = LCD_CLK_SRC_DEFAULT;
  c.timings.pclk_hz = PANEL_PCLK_HZ;
  c.timings.h_res = SCREEN_W;
  c.timings.v_res = SCREEN_H;
  c.timings.hsync_pulse_width = 4;
  c.timings.hsync_back_porch = 8;
  c.timings.hsync_front_porch = 8;
  c.timings.vsync_pulse_width = 4;
  c.timings.vsync_back_porch = 8;
  c.timings.vsync_front_porch = 8;
  c.timings.flags.hsync_idle_low = 1;
  c.timings.flags.vsync_idle_low = 1;
  c.timings.flags.pclk_active_neg = 1;
  c.data_width = 16;                                // RGB565, 16 data lines
  c.bits_per_pixel = 16;
  c.num_fbs = 1;
  c.bounce_buffer_size_px = PANEL_BOUNCE_PX;
  c.dma_burst_size = 64;
  c.hsync_gpio_num = 39;
  c.vsync_gpio_num = 41;
  c.de_gpio_num = 40;
  c.pclk_gpio_num = 42;
  c.disp_gpio_num = -1;
  static const int8_t DATA[16] = {8, 3, 46, 9, 1,          // B0-B4
                                  5, 6, 7, 15, 16, 4,      // G0-G5
                                  45, 48, 47, 21, 14};     // R0-R4
  for (int i = 0; i < 16; i++) c.data_gpio_nums[i] = DATA[i];
  c.flags.fb_in_psram = 1;
  esp_err_t e = esp_lcd_new_rgb_panel(&c, &g_panel);
  if (e == ESP_OK) e = esp_lcd_panel_reset(g_panel);
  if (e == ESP_OK) e = esp_lcd_panel_init(g_panel);
  void *fb = NULL;
  if (e == ESP_OK) e = esp_lcd_rgb_panel_get_frame_buffer(g_panel, 1, &fb);
  if (e != ESP_OK) { printf("panel: not started (%s)\n", esp_err_to_name(e)); return; }
  g_fb = (uint16_t *)fb;
  memset(g_fb, 0, (size_t)SCREEN_W * SCREEN_H * 2);
  Cache_WriteBack_Addr((uint32_t)g_fb, (uint32_t)SCREEN_W * SCREEN_H * 2);
  pin_set(PIN_BL, 1);
}
