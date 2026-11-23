// Display part of the board layer (brewdeck_core.h) for the Sunton ESP32-S3 4.3" (800x480 RGB panel): on top of
// panel.cpp. The panel is driven by Arduino_GFX with its frame buffer in PSRAM and bounce buffers (see panel.cpp);
// a flushed rectangle is copied into that frame buffer by the CPU. Touch, memory: src/board/common.
#include <Arduino.h>
#include "board.h"
#include "soc/spi_mem_reg.h"
#include "../../core/brewdeck_core.h"
#include "board_core.h"

void board_serial_begin(void) { Serial.begin(115200); }   // UART0 through the board's USB-serial chip
const char *board_hostname(void) { return "brewdeck"; }

static const bd_display_info g_info = {SCREEN_W, SCREEN_H, BD_PIXEL_RGB565, 40};   // one 40-line strip: 64 KB internal RAM
const bd_display_info *bd_display(void) { return &g_info; }

// The flushed rectangles are copied by the CPU. A DMA copy (esp_async_memcpy) was 10 ms faster per full screen but
// scrambled the picture on the panel: its writes to PSRAM compete with the panel's bounce-buffer reads from the same
// frame buffer, and the panel lost sync.
static uint32_t g_flush_us = 0, g_flush_px = 0;   // time spent copying into the frame buffer (bd_display_stats)
void bd_display_flush(int x1, int y1, int x2, int y2, const uint16_t *px) {
  const uint32_t w = (uint32_t)(x2 - x1 + 1), h = (uint32_t)(y2 - y1 + 1);
  const uint32_t t0 = micros();
  gfx->draw16bitRGBBitmap(x1, y1, (uint16_t *)px, w, h);
  g_flush_us += micros() - t0;
  g_flush_px += w * h;
}
void bd_display_stats(uint32_t *us, uint32_t *px, bool reset) {
  if (us) *us = g_flush_us;
  if (px) *px = g_flush_px;
  if (reset) g_flush_us = g_flush_px = 0;
}

// the frame buffer is landscape already: its rows are the picture's rows
const uint16_t *board_screen_rows(int y, int n) {
  uint16_t *fb = gfx ? gfx->getFramebuffer() : NULL;
  return fb && y >= 0 && n > 0 && y + n <= SCREEN_H ? fb + (size_t)y * SCREEN_W : NULL;
}

const char *board_display_mode(void) { return "copied by the CPU (landscape panel, no rotation)"; }

const char *board_flash_mode(uint32_t *ctrl) {
  const uint32_t c = REG_READ(SPI_MEM_CTRL_REG(0));
  if (ctrl) *ctrl = c;
  return c & SPI_MEM_FREAD_QIO ? "QIO" : c & SPI_MEM_FREAD_QUAD ? "QOUT" : c & SPI_MEM_FREAD_DIO ? "DIO" :
         c & SPI_MEM_FREAD_DUAL ? "DOUT" : "single";
}
