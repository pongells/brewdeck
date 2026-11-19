// Display part of the board layer (brewdeck_core.h) for the Guition JC4880P443 (ESP32-P4): the UI's 800x480 picture
// on the 480x800 portrait panel (panel.cpp). Arduino_GFX keeps the panel's frame buffer in PSRAM (the DSI controller
// reads it by DMA) and, with rotation PANEL_ROT, turns each flushed rectangle into it and writes the cache back.
// Touch, memory: src/board/common. Not done yet: the P4's 2D engine (PPA) for the rotated copy (the CPU does it).
#include <Arduino.h>
#include <esp_heap_caps.h>
#include "soc/spi_mem_c_reg.h"
#include "board.h"
#include "../../core/brewdeck_core.h"
#include "board_core.h"

const char *board_hostname(void) { return "brewdeck-p4"; }   // brewdeck-p4.local: the S3 screen keeps "brewdeck"

// Serial is the chip's USB port. When the computer on the other end does not read it, every write waits up to 20 x
// the TX timeout (2 s at the default 100 ms), and the UI, network and WebSocket tasks all stop on the log (seen:
// 30 s freezes). Timeout 0: lines nobody reads are dropped instead; a larger buffer for bursts.

void board_serial_begin(void) {
  Serial.setTxBufferSize(4096);
  Serial.begin(115200);
  Serial.setTxTimeoutMs(0);
}

static const bd_display_info g_info = {SCREEN_W, SCREEN_H, BD_PIXEL_RGB565, 40};
const bd_display_info *bd_display(void) { return &g_info; }

static uint32_t g_flush_us = 0, g_flush_px = 0;   // time spent copying into the frame buffer (bd_display_stats)
void bd_display_flush(int x1, int y1, int x2, int y2, const uint16_t *px) {
  const uint32_t w = (uint32_t)(x2 - x1 + 1), h = (uint32_t)(y2 - y1 + 1);
  const uint32_t t0 = micros();
  gfx->draw16bitRGBBitmap(x1, y1, (uint16_t *)px, w, h);   // rotated into the portrait frame buffer + cache write-back
  g_flush_us += micros() - t0;
  g_flush_px += w * h;
}
void bd_display_stats(uint32_t *us, uint32_t *px, bool reset) {
  if (us) *us = g_flush_us;
  if (px) *px = g_flush_px;
  if (reset) g_flush_us = g_flush_px = 0;
}

// screenshots: the frame buffer is portrait, so gather the landscape rows (the inverse of the rotation) into a buffer
const uint16_t *board_screen_rows(int y, int n) {
  static uint16_t *buf = NULL;
  uint16_t *fb = gfx->getFramebuffer();
  if (!fb || y < 0 || n <= 0 || n > 8 || y + n > SCREEN_H) return NULL;
  if (!buf) buf = (uint16_t *)heap_caps_malloc(SCREEN_W * 8 * 2, MALLOC_CAP_SPIRAM);
  if (!buf) return NULL;
  for (int r = 0; r < n; r++)
    for (int x = 0; x < SCREEN_W; x++) {
#if PANEL_ROT == 1
      const int px = PANEL_W - 1 - (y + r), py = x;   // picture (x, y) -> panel (479 - y, x)
#else
      const int px = y + r, py = PANEL_H - 1 - x;     // picture (x, y) -> panel (y, 799 - x)
#endif
      buf[r * SCREEN_W + x] = fb[(size_t)py * PANEL_W + px];
    }
  return buf;
}

// the P4's flash controller (SPI0) names its registers SPI_MEM_C_*
const char *board_flash_mode(uint32_t *ctrl) {
  const uint32_t c = REG_READ(SPI_MEM_C_CTRL_REG);
  if (ctrl) *ctrl = c;
  return c & SPI_MEM_C_FREAD_QIO ? "QIO" : c & SPI_MEM_C_FREAD_QUAD ? "QOUT" : c & SPI_MEM_C_FREAD_DIO ? "DIO" :
         c & SPI_MEM_C_FREAD_DUAL ? "DOUT" : "single";
}
