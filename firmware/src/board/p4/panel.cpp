// Guition JC4880P443 panel on ESP-IDF's MIPI-DSI driver (esp_lcd): 2 data lanes to the ST7701 controller, one
// portrait frame buffer in PSRAM that the DSI host scans out on its own (DPI video mode). Timing, lane rate and the
// ST7701 start-up commands as Arduino_GFX's JC4880P433 example had them.
#include <string.h>
#include <esp_cache.h>
#include <esp_ldo_regulator.h>
#include <esp_lcd_mipi_dsi.h>
#include <esp_lcd_panel_ops.h>
#include <esp_lcd_panel_io.h>
#include "board.h"
#include "board_pins.h"
#include "../../core/systime.h"

// ST7701 start-up: {command, data, length, wait in ms after it}
struct InitCmd { uint8_t cmd; uint8_t data[16]; uint8_t n; uint16_t ms; };
static const InitCmd ST7701_INIT[] = {
    {0xFF, {0x77, 0x01, 0x00, 0x00, 0x13}, 5, 0},
    {0xEF, {0x08}, 1, 0},
    {0xFF, {0x77, 0x01, 0x00, 0x00, 0x10}, 5, 0},
    {0xC0, {0x63, 0x00}, 2, 0},
    {0xC1, {0x0D, 0x02}, 2, 0},
    {0xC2, {0x10, 0x08}, 2, 0},
    {0xCC, {0x10}, 1, 0},
    {0xB0, {0x80, 0x09, 0x53, 0x0C, 0xD0, 0x07, 0x0C, 0x09, 0x09, 0x28, 0x06, 0xD4, 0x13, 0x69, 0x2B, 0x71}, 16, 0},
    {0xB1, {0x80, 0x94, 0x5A, 0x10, 0xD3, 0x06, 0x0A, 0x08, 0x08, 0x25, 0x03, 0xD3, 0x12, 0x66, 0x6A, 0x0D}, 16, 0},
    {0xFF, {0x77, 0x01, 0x00, 0x00, 0x11}, 5, 0},
    {0xB0, {0x5D}, 1, 0},
    {0xB1, {0x58}, 1, 0},
    {0xB2, {0x87}, 1, 0},
    {0xB3, {0x80}, 1, 0},
    {0xB5, {0x4E}, 1, 0},
    {0xB7, {0x85}, 1, 0},
    {0xB8, {0x21}, 1, 0},
    {0xB9, {0x10, 0x1F}, 2, 0},
    {0xBB, {0x03}, 1, 0},
    {0xBC, {0x00}, 1, 0},
    {0xC1, {0x78}, 1, 0},
    {0xC2, {0x78}, 1, 0},
    {0xD0, {0x88}, 1, 0},
    {0xE0, {0x00, 0x3A, 0x02}, 3, 0},
    {0xE1, {0x04, 0xA0, 0x00, 0xA0, 0x05, 0xA0, 0x00, 0xA0, 0x00, 0x40, 0x40}, 11, 0},
    {0xE2, {0x30, 0x00, 0x40, 0x40, 0x32, 0xA0, 0x00, 0xA0, 0x00, 0xA0, 0x00, 0xA0, 0x00}, 13, 0},
    {0xE3, {0x00, 0x00, 0x33, 0x33}, 4, 0},
    {0xE4, {0x44, 0x44}, 2, 0},
    {0xE5, {0x09, 0x2E, 0xA0, 0xA0, 0x0B, 0x30, 0xA0, 0xA0, 0x05, 0x2A, 0xA0, 0xA0, 0x07, 0x2C, 0xA0, 0xA0}, 16, 0},
    {0xE6, {0x00, 0x00, 0x33, 0x33}, 4, 0},
    {0xE7, {0x44, 0x44}, 2, 0},
    {0xE8, {0x08, 0x2D, 0xA0, 0xA0, 0x0A, 0x2F, 0xA0, 0xA0, 0x04, 0x29, 0xA0, 0xA0, 0x06, 0x2B, 0xA0, 0xA0}, 16, 0},
    {0xEB, {0x00, 0x00, 0x4E, 0x4E, 0x00, 0x00, 0x00}, 7, 0},
    {0xEC, {0x08, 0x01}, 2, 0},
    {0xED, {0xB0, 0x2B, 0x98, 0xA4, 0x56, 0x7F, 0xFF, 0xFF, 0xFF, 0xFF, 0xF7, 0x65, 0x4A, 0x89, 0xB2, 0x0B}, 16, 0},
    {0xEF, {0x08, 0x08, 0x08, 0x45, 0x3F, 0x54}, 6, 0},
    {0xFF, {0x77, 0x01, 0x00, 0x00, 0x00}, 5, 0},
    {0x11, {0x00}, 1, 120},   // sleep out
    {0x29, {0x00}, 1, 20},    // display on
};

static esp_lcd_panel_handle_t g_panel;
static uint16_t *g_fb;
uint16_t *board_fb(void) { return g_fb; }

static bool panel_start(void) {
  // the DSI PHY's supply: on-chip LDO channel 3 at 2.5 V (VDD_MIPI_DPHY on this board)
  static esp_ldo_channel_handle_t ldo;
  esp_ldo_channel_config_t lc = {};
  lc.chan_id = 3;
  lc.voltage_mv = 2500;
  esp_err_t e = esp_ldo_acquire_channel(&lc, &ldo);
  esp_lcd_dsi_bus_config_t bc = {};
  bc.bus_id = 0;
  bc.num_data_lanes = 2;
  bc.phy_clk_src = MIPI_DSI_PHY_PLLREF_CLK_SRC_DEFAULT;
  bc.lane_bit_rate_mbps = 750;
  esp_lcd_dsi_bus_handle_t bus = NULL;
  if (e == ESP_OK) e = esp_lcd_new_dsi_bus(&bc, &bus);
  esp_lcd_dbi_io_config_t ic = {};
  ic.virtual_channel = 0;
  ic.lcd_cmd_bits = 8;
  ic.lcd_param_bits = 8;
  esp_lcd_panel_io_handle_t io = NULL;
  if (e == ESP_OK) e = esp_lcd_new_panel_io_dbi(bus, &ic, &io);
  esp_lcd_dpi_panel_config_t dc = {};
  dc.virtual_channel = 0;
  dc.dpi_clk_src = MIPI_DSI_DPI_CLK_SRC_DEFAULT;
  dc.dpi_clock_freq_mhz = 34;
  dc.pixel_format = LCD_COLOR_PIXEL_FORMAT_RGB565;
  dc.num_fbs = 1;
  dc.video_timing.h_size = PANEL_W;
  dc.video_timing.v_size = PANEL_H;
  dc.video_timing.hsync_pulse_width = 12;
  dc.video_timing.hsync_back_porch = 42;
  dc.video_timing.hsync_front_porch = 42;
  dc.video_timing.vsync_pulse_width = 2;
  dc.video_timing.vsync_back_porch = 8;
  dc.video_timing.vsync_front_porch = 166;
  dc.flags.use_dma2d = true;
  if (e == ESP_OK) e = esp_lcd_new_panel_dpi(bus, &dc, &g_panel);
  for (size_t i = 0; e == ESP_OK && i < sizeof ST7701_INIT / sizeof ST7701_INIT[0]; i++) {
    e = esp_lcd_panel_io_tx_param(io, ST7701_INIT[i].cmd, ST7701_INIT[i].data, ST7701_INIT[i].n);
    if (ST7701_INIT[i].ms) wait_ms(ST7701_INIT[i].ms);
  }
  if (e == ESP_OK) e = esp_lcd_panel_init(g_panel);
  void *fb = NULL;
  if (e == ESP_OK) e = esp_lcd_dpi_panel_get_frame_buffer(g_panel, 1, &fb);
  if (e != ESP_OK) { printf("panel: not started (%s)\n", esp_err_to_name(e)); return false; }
  g_fb = (uint16_t *)fb;
  return true;
}

void board_init() {
  pin_output(PIN_BL, 0);   // keep dark until the first frame is drawn
  pin_output(PIN_PANEL_RST, 1);                     // reset pulse
  wait_ms(5);
  pin_set(PIN_PANEL_RST, 0);
  wait_ms(10);
  pin_set(PIN_PANEL_RST, 1);
  wait_ms(120);
  if (!panel_start()) return;
  memset(g_fb, 0, (size_t)PANEL_W * PANEL_H * 2);
  esp_cache_msync(g_fb, (size_t)PANEL_W * PANEL_H * 2, ESP_CACHE_MSYNC_FLAG_DIR_C2M | ESP_CACHE_MSYNC_FLAG_UNALIGNED);
  wait_ms(50);                   // ~3 frames of the black fill first: lit at once, the panel showed its blue for a frame
  pin_set(PIN_BL, 1);
}
