// Display part of the board layer (brewdeck_core.h) for the Guition JC4880P443 (ESP32-P4): the UI's 800x480 picture
// on the 480x800 portrait panel (panel.cpp). Arduino_GFX keeps the panel's frame buffer in PSRAM (the DSI controller
// reads it by DMA). Each flushed rectangle is turned into it by the P4's 2D engine (PPA, rotate by 90 / 270 degrees);
// if the PPA cannot be used, Arduino_GFX does the same with the CPU (rotation PANEL_ROT + cache write-back).
// Build with -DP4_PPA=0 to compare the two. Touch, memory: src/board/common.
#include <Arduino.h>
#include <esp_heap_caps.h>
#include <driver/ppa.h>
#include <esp_private/esp_cache_private.h>   // esp_cache_get_alignment (what the PPA driver checks too)
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

#ifndef P4_PPA
#define P4_PPA 1
#endif

// PPA client for the rotated copy; NULL = use the CPU path. Set up on the first flush (the frame buffer exists then).
static ppa_client_handle_t g_ppa = NULL;
static bool g_ppa_tried = false;
static const char *g_disp_mode = "rotated by the CPU";   // board_display_mode

static void ppa_setup(void) {
  g_ppa_tried = true;
  if (!P4_PPA) return;
  uint16_t *fb = gfx->getFramebuffer();
  // the PPA writes the frame buffer by DMA: it must start and end on a cache line (the size IDF asks for in PSRAM)
  size_t align = 0;
  if (esp_cache_get_alignment(MALLOC_CAP_SPIRAM | MALLOC_CAP_DMA, &align) != ESP_OK || !align) align = 128;
  if (!fb || ((uintptr_t)fb % align) || (PANEL_W * PANEL_H * 2) % align) {
    static char why[96];
    snprintf(why, sizeof why, "rotated by the CPU (frame buffer at %p, not on a %u-byte line for the PPA)", fb,
             (unsigned)align);
    g_disp_mode = why;
    Serial.printf("board: PPA not used, %s\n", why);
    return;
  }
  ppa_client_config_t cc = {};
  cc.oper_type = PPA_OPERATION_SRM;
  cc.max_pending_trans_num = 1;
  if (ppa_register_client(&cc, &g_ppa) != ESP_OK) {
    g_ppa = NULL;
    g_disp_mode = "rotated by the CPU (PPA not available)";
    Serial.println("board: PPA not available; rotating with the CPU");
  } else {
    g_disp_mode = "rotated by the PPA";
    Serial.println("board: rotating with the PPA");
  }
}

// Rotate one rectangle of the landscape picture into the portrait frame buffer with the PPA. Blocking: the UI reuses
// px as soon as this returns. The PPA keeps the caches right for both buffers.
static bool ppa_flush(int x1, int y1, uint32_t w, uint32_t h, const uint16_t *px) {
  ppa_srm_oper_config_t op = {};
  op.in.buffer = px;
  op.in.pic_w = w;
  op.in.pic_h = h;
  op.in.block_w = w;
  op.in.block_h = h;
  op.in.srm_cm = PPA_SRM_COLOR_MODE_RGB565;
  op.out.buffer = gfx->getFramebuffer();
  op.out.buffer_size = PANEL_W * PANEL_H * 2;
  op.out.pic_w = PANEL_W;
  op.out.pic_h = PANEL_H;
  op.out.srm_cm = PPA_SRM_COLOR_MODE_RGB565;
#if PANEL_ROT == 1
  // picture (x, y) -> panel (479 - y, x): a quarter turn clockwise = 270 counter-clockwise
  op.rotation_angle = PPA_SRM_ROTATION_ANGLE_270;
  op.out.block_offset_x = PANEL_W - (y1 + h);
  op.out.block_offset_y = x1;
#else
  // picture (x, y) -> panel (y, 799 - x): a quarter turn counter-clockwise
  op.rotation_angle = PPA_SRM_ROTATION_ANGLE_90;
  op.out.block_offset_x = y1;
  op.out.block_offset_y = PANEL_H - (x1 + w);
#endif
  op.scale_x = 1.0f;
  op.scale_y = 1.0f;
  op.mode = PPA_TRANS_MODE_BLOCKING;
  return ppa_do_scale_rotate_mirror(g_ppa, &op) == ESP_OK;
}

static uint32_t g_flush_us = 0, g_flush_px = 0;   // time spent copying into the frame buffer (bd_display_stats)
void bd_display_flush(int x1, int y1, int x2, int y2, const uint16_t *px) {
  const uint32_t w = (uint32_t)(x2 - x1 + 1), h = (uint32_t)(y2 - y1 + 1);
  const uint32_t t0 = micros();
  if (!g_ppa_tried) ppa_setup();
  if (!g_ppa || !ppa_flush(x1, y1, w, h, px)) {
    if (g_ppa) {   // a PPA error: say so once and stay on the CPU path
      g_disp_mode = "rotated by the CPU (a PPA copy failed)";
      Serial.println("board: PPA rotation failed; rotating with the CPU from now on");
      ppa_unregister_client(g_ppa);
      g_ppa = NULL;
    }
    gfx->draw16bitRGBBitmap(x1, y1, (uint16_t *)px, w, h);   // rotated by the CPU + cache write-back
  }
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

const char *board_display_mode(void) { return g_disp_mode; }

// the P4's flash controller (SPI0) names its registers SPI_MEM_C_*
const char *board_flash_mode(uint32_t *ctrl) {
  const uint32_t c = REG_READ(SPI_MEM_C_CTRL_REG);
  if (ctrl) *ctrl = c;
  return c & SPI_MEM_C_FREAD_QIO ? "QIO" : c & SPI_MEM_C_FREAD_QUAD ? "QOUT" : c & SPI_MEM_C_FREAD_DIO ? "DIO" :
         c & SPI_MEM_C_FREAD_DUAL ? "DOUT" : "single";
}
