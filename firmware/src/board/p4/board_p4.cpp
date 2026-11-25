// Display part of the board layer (brewdeck_core.h) for the Guition JC4880P443 (ESP32-P4): the UI's 800x480 picture
// on the 480x800 portrait panel (panel.cpp). Arduino_GFX keeps the panel's frame buffer in PSRAM (the DSI controller
// reads it by DMA). Each flushed rectangle is turned into it by the P4's 2D engine (PPA, rotate by 90 / 270 degrees);
// if the PPA cannot be used, Arduino_GFX does the same with the CPU (rotation PANEL_ROT + cache write-back).
// Build with -DP4_PPA=0 to compare the two. Touch, memory: src/board/common.
#include <Arduino.h>
#include <esp_heap_caps.h>
#include <driver/ppa.h>
#include <driver/usb_serial_jtag.h>
#include <driver/usb_serial_jtag_vfs.h>
#include <freertos/semphr.h>
#include <esp_cache.h>
#include <esp_hosted.h>
#include <esp_timer.h>
#include <esp_private/esp_cache_private.h>   // esp_cache_get_alignment (what the PPA driver checks too)
#include "soc/spi_mem_c_reg.h"
#include "board.h"
#include "../../core/brewdeck_core.h"
#include "board_core.h"

const char *board_hostname(void) { return "brewdeck-p4"; }   // brewdeck-p4.local: the S3 screen keeps "brewdeck"

// Wi-Fi is the ESP32-C6's, reached over SDIO through ESP-Hosted (esp_wifi_* calls become requests to it). The SDIO
// pins come from the ESP-IDF settings (they match this board). ESP-Hosted's own start at boot is a constructor in an
// object nothing references, so the linker leaves it out: start it here, then wait for the C6.
bool board_wifi_init(void) {
  const int64_t t0 = esp_timer_get_time();
  esp_err_t e = esp_hosted_init();
  if (e == ESP_OK) e = esp_hosted_connect_to_slave();
  if (e != ESP_OK) printf("wifi: no link to the ESP32-C6 (%s)\n", esp_err_to_name(e));
  else printf("wifi: ESP32-C6 link up in %d ms\n", (int)((esp_timer_get_time() - t0) / 1000));
  return e == ESP_OK;
}

// The log goes to the chip's USB port (USB-Serial-JTAG). When the computer on the other end does not read it, a
// waiting write stops every task that logs (seen with Arduino's driver: 30 s freezes), so the log never waits: what
// does not fit in the 4 KB buffer is dropped. ESP-IDF's console (its own lines) uses the same driver.
void board_serial_begin(void) {
  usb_serial_jtag_driver_config_t c = {4096, 256};
  if (usb_serial_jtag_driver_install(&c) == ESP_OK) usb_serial_jtag_vfs_use_driver();
}
void board_log_write(const char *b, size_t n) { usb_serial_jtag_write_bytes(b, n, 0); }

// 48-row strips (75 KB internal RAM) and redrawn areas in whole 16-row bands: LVGL then flushes only rectangles that
// are multiples of 16 rows tall, which the PPA copies safely (see bd_display_flush)
#define P4_ROWS 16
static const bd_display_info g_info = {SCREEN_W, SCREEN_H, BD_PIXEL_RGB565, 48, P4_ROWS};
const bd_display_info *bd_display(void) { return &g_info; }

#ifndef P4_PPA
#define P4_PPA 1
#endif

// PPA client for the rotated copy; NULL = use the CPU path. Set up on the first flush (the frame buffer exists then).
static ppa_client_handle_t g_ppa = NULL;
static bool g_ppa_tried = false;
static size_t g_align = 128;   // cache line of the frame buffer (esp_cache_get_alignment)
static const char *g_disp_mode = "rotated by the CPU";   // board_display_mode

static bool ppa_worker_start(void);
static void ppa_setup(void) {
  g_ppa_tried = true;
  if (!P4_PPA) return;
  uint16_t *fb = gfx->getFramebuffer();
  // the PPA writes the frame buffer by DMA: it must start and end on a cache line (the size IDF asks for in PSRAM)
  size_t align = 0;
  if (esp_cache_get_alignment(MALLOC_CAP_SPIRAM | MALLOC_CAP_DMA, &align) != ESP_OK || !align) align = 128;
  g_align = align;
  if (!fb || ((uintptr_t)fb % align) || (PANEL_W * PANEL_H * 2) % align) {
    static char why[96];
    snprintf(why, sizeof why, "rotated by the CPU (frame buffer at %p, not on a %u-byte line for the PPA)", fb,
             (unsigned)align);
    g_disp_mode = why;
    printf("board: PPA not used, %s\n", why);
    return;
  }
  ppa_client_config_t cc = {};
  cc.oper_type = PPA_OPERATION_SRM;
  cc.max_pending_trans_num = 1;
  if (ppa_register_client(&cc, &g_ppa) != ESP_OK) {
    g_ppa = NULL;
    g_disp_mode = "rotated by the CPU (PPA not available)";
    printf("board: PPA not available; rotating with the CPU\n");
  } else if (!ppa_worker_start()) {
    ppa_unregister_client(g_ppa);
    g_ppa = NULL;
    g_disp_mode = "rotated by the CPU (no memory for the PPA task)";
  } else {
    g_disp_mode = "rotated by the PPA";
    printf("board: rotating with the PPA\n");
  }
}

// Rotate one rectangle of the landscape picture into the portrait frame buffer with the PPA. Blocking: the UI reuses
// px as soon as this returns. The PPA keeps the caches right for both buffers.
static void ppa_op(ppa_srm_oper_config_t &op, int x1, int y1, uint32_t w, uint32_t h, const uint16_t *px,
                   uint16_t *out) {
  op = {};
  op.in.buffer = px;
  op.in.pic_w = w;
  op.in.pic_h = h;
  op.in.block_w = w;
  op.in.block_h = h;
  op.in.srm_cm = PPA_SRM_COLOR_MODE_RGB565;
  op.out.buffer = out;
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
}
static bool ppa_flush(int x1, int y1, uint32_t w, uint32_t h, const uint16_t *px) {
  ppa_srm_oper_config_t op;
  ppa_op(op, x1, y1, w, h, px, gfx->getFramebuffer());
  op.mode = PPA_TRANS_MODE_BLOCKING;
  return ppa_do_scale_rotate_mirror(g_ppa, &op) == ESP_OK;
}

static void pt_map(int x, int y, int &px, int &py) {   // picture -> panel, as ppa_flush and board_screen_rows
#if PANEL_ROT == 1
  px = PANEL_W - 1 - y, py = x;
#else
  px = y, py = PANEL_H - 1 - x;
#endif
}
static void pt_sync(uint16_t *fb, size_t a, size_t b, size_t align, int dir) {   // pixels [a, b) of fb, whole lines
  uintptr_t s = (uintptr_t)(fb + a) & ~(uintptr_t)(align - 1);
  uintptr_t e = ((uintptr_t)(fb + b) + align - 1) & ~(uintptr_t)(align - 1);
  esp_cache_msync((void *)s, e - s, dir);
}
static uint32_t g_flush_us = 0, g_flush_px = 0;   // time spent copying into the frame buffer (bd_display_stats)
static volatile int g_fl_x = 0, g_fl_y = 0, g_fl_w = 0, g_fl_h = 0;   // the last flush (board_flush_info)
static volatile bool g_fl_busy = false;
static const char *volatile g_fl_how = "";
void board_flush_info(char *out, size_t cap) {
  snprintf(out, cap, "last flush %d,%d %dx%d by the %s%s", g_fl_x, g_fl_y, g_fl_w, g_fl_h, g_fl_how,
           g_fl_busy ? ", STILL COPYING" : ", finished");
}
// The PPA has known bugs on the P4 (ESP-IDF issues 19023, 19096, 19462, 18999, PR 19047): a rotation during DSI
// scanout sometimes never finishes, for some sizes and positions and also at random under load, and the driver
// neither notices nor recovers; the copy that hung is lost and the engine stays stuck. So: rectangles it is known to
// trip on (thinner than P4_ROWS, output x not on a multiple of 4) go to the CPU; the others go to a helper task on
// the UI's core (submitting from another core is one of the bugs) and the UI waits at most PPA_WAIT_MS for it. A
// copy that does not finish (inside the driver call or after it) is redone by the CPU, which rotates from then on
// until a restart; the helper task stays stuck, harmless. GET /heap ("display:") and the log say so.
static const uint32_t PPA_WAIT_MS = 100;   // a copy takes ~0.5-6 ms
static SemaphoreHandle_t g_job_go, g_job_done;
static struct { int x1, y1; uint32_t w, h; const uint16_t *px; bool ok; } g_job;
static uint32_t g_cpu_flushes = 0;         // rectangles the CPU rotated while the PPA is in use (thin or unaligned)
static bool g_ppa_lost = false;
static volatile bool g_ppa_stall = false;    // debug (GET /ppatest?stall=1): the next copy hangs, as a real one would
void board_ppa_stall(void) { g_ppa_stall = true; }
static void ppa_worker(void *) {
  for (;;) {
    xSemaphoreTake(g_job_go, portMAX_DELAY);
    while (g_ppa_stall) vTaskDelay(pdMS_TO_TICKS(1000));
    g_job.ok = ppa_flush(g_job.x1, g_job.y1, g_job.w, g_job.h, g_job.px);
    xSemaphoreGive(g_job_done);
  }
}
static bool ppa_worker_start(void) {
  g_job_go = xSemaphoreCreateBinary();
  g_job_done = xSemaphoreCreateBinary();
  return g_job_go && g_job_done &&
         xTaskCreatePinnedToCore(ppa_worker, "ppa", 3072, NULL, 5, NULL, xPortGetCoreID()) == pdPASS;
}
// true when the PPA rotated it; false: not finished in time (the PPA is then given up) or refused by the driver
static bool ppa_copy(int x1, int y1, uint32_t w, uint32_t h, const uint16_t *px) {
  g_job = {x1, y1, w, h, px, false};
  xSemaphoreTake(g_job_done, 0);                 // no stale "done"
  xSemaphoreGive(g_job_go);
  if (xSemaphoreTake(g_job_done, pdMS_TO_TICKS(PPA_WAIT_MS)) == pdTRUE) return g_job.ok;
  g_ppa_lost = true;
  g_ppa = NULL;                                  // not unregistered: its stuck copy is still queued
  g_disp_mode = "rotated by the CPU (a PPA copy never finished)";
  bd_log("board: a PPA copy (%d,%d %lux%lu) did not finish in %lu ms; rotating with the CPU until a restart", x1, y1,
         (unsigned long)w, (unsigned long)h, (unsigned long)PPA_WAIT_MS);
  return false;
}
// the CPU's rotation; its frame buffer lines are dropped from the cache first: the PPA may have written them behind
// the cache, and the CPU's write-back of a stale line would undo the PPA's pixels in it
static void cpu_copy(int x1, int y1, uint32_t w, uint32_t h, const uint16_t *px) {
  int ax, ay, bx, by;
  pt_map(x1, y1, ax, ay);
  pt_map(x1 + w - 1, y1 + h - 1, bx, by);
  pt_sync(gfx->getFramebuffer(), (size_t)min(ay, by) * PANEL_W, (size_t)(max(ay, by) + 1) * PANEL_W, g_align,
          ESP_CACHE_MSYNC_FLAG_DIR_M2C);
  gfx->draw16bitRGBBitmap(x1, y1, (uint16_t *)px, w, h);   // rotated by the CPU + cache write-back
}
static bool ppa_suits(int y1, uint32_t h) {     // sizes and positions the PPA is known to trip on: CPU
#if PANEL_ROT == 1
  const int ox = PANEL_W - (y1 + (int)h);
#else
  const int ox = y1;
#endif
  return h >= P4_ROWS && ox % 4 == 0;
}

void bd_display_flush(int x1, int y1, int x2, int y2, const uint16_t *px) {
  const uint32_t w = (uint32_t)(x2 - x1 + 1), h = (uint32_t)(y2 - y1 + 1);
  if (!g_ppa_tried) ppa_setup();
  const bool use_ppa = g_ppa && ppa_suits(y1, h);
  g_fl_x = x1, g_fl_y = y1, g_fl_w = (int)w, g_fl_h = (int)h;
  g_fl_how = use_ppa ? "PPA" : "CPU";
  g_fl_busy = true;
  const uint32_t t0 = micros();
  if (!use_ppa || !ppa_copy(x1, y1, w, h, px)) {
    if (g_ppa) g_cpu_flushes++;
    cpu_copy(x1, y1, w, h, px);
  }
  g_fl_busy = false;
  g_flush_us += micros() - t0;
  g_flush_px += w * h;
}
// Debug (GET /ppatest): rotates random rectangles of the sizes LVGL flushes (up to 800 x draw_lines) with the PPA
// into a spare frame buffer, with the same settings as ppa_flush, and checks each against the CPU's rotation: the
// rectangle's pixels and that nothing around it changed. A copy that does not finish within 500 ms is reported and
// the test stops (the engine may stay stuck: the display then rotates with the CPU). Never touches the panel.
static int g_pt_n = 300;
static uint32_t g_pt_seed = 1;
static uint32_t g_pt_h0 = 1, g_pt_h1 = 0;   // rectangle heights h0..h1 (0 = draw_lines)
void board_ppa_test_args(int n, uint32_t seed, int hmin, int hmax) {
  g_pt_n = n < 1 ? 1 : n > 5000 ? 5000 : n;
  g_pt_seed = seed ? seed : 1;
  g_pt_h1 = hmax > 0 && hmax <= g_info.draw_lines ? hmax : g_info.draw_lines;
  g_pt_h0 = hmin > 0 && hmin <= (int)g_pt_h1 ? hmin : 1;
}
static bool IRAM_ATTR pt_done(ppa_client_handle_t, ppa_event_data_t *, void *flag) {
  *(volatile bool *)flag = true;
  return false;
}
static uint32_t pt_rand(uint32_t &s) {   // xorshift32: the same seed gives the same rectangles
  s ^= s << 13;
  s ^= s >> 17;
  s ^= s << 5;
  return s;
}
size_t board_ppa_test(char *out, size_t cap) {
  size_t n = 0;
#define PT_SAY(...) \
  do { \
    if (n + 1 < cap) n += snprintf(out + n, cap - n, __VA_ARGS__); \
    if (n >= cap) n = cap - 1; \
  } while (0)
  if (!g_ppa_tried) ppa_setup();
  size_t align = 0;
  if (esp_cache_get_alignment(MALLOC_CAP_SPIRAM | MALLOC_CAP_DMA, &align) != ESP_OK || !align) align = 128;
  const size_t fb_px = (size_t)PANEL_W * PANEL_H, src_px = (size_t)SCREEN_W * g_info.draw_lines;
  static uint16_t *fb = NULL;   // kept between runs (768 KB of PSRAM)
  if (!fb) fb = (uint16_t *)heap_caps_aligned_calloc(align, fb_px, 2, MALLOC_CAP_SPIRAM);
  bool src_int = true;          // like LVGL's draw strip (bd_lvgl9.h): internal RAM, PSRAM if short
  uint16_t *src = (uint16_t *)heap_caps_malloc(src_px * 2, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
  if (!src) src = (uint16_t *)heap_caps_malloc(src_px * 2, MALLOC_CAP_SPIRAM), src_int = false;
  ppa_client_handle_t cl = NULL;
  ppa_client_config_t cc = {};
  cc.oper_type = PPA_OPERATION_SRM;
  cc.max_pending_trans_num = 1;
  if (!fb || !src || ppa_register_client(&cc, &cl) != ESP_OK) {
    PT_SAY("ppa test: no memory or no PPA client\n");
    free(src);
    return n;
  }
  ppa_event_callbacks_t cbs = {};
  cbs.on_trans_done = pt_done;
  ppa_client_register_event_callbacks(cl, &cbs);
  memset(fb, 0, fb_px * 2);     // 0 = untouched; test pixels are odd, so never 0
  pt_sync(fb, 0, fb_px, align, ESP_CACHE_MSYNC_FLAG_DIR_C2M);

  uint32_t s = g_pt_seed, ok = 0, bad = 0, err = 0, us = 0, listed = 0;
  int done = 0;
  bool hung = false;
  PT_SAY("ppa test: %d rectangles, heights %lu-%lu, seed %lu, source in %s, %s\n", g_pt_n, (unsigned long)g_pt_h0,
         (unsigned long)(g_pt_h1 ? g_pt_h1 : g_info.draw_lines), (unsigned long)g_pt_seed,
         src_int ? "internal RAM" : "PSRAM", g_disp_mode);
  const size_t head = n;
  for (; done < g_pt_n && !hung; done++) {
    const uint32_t kind = pt_rand(s) % 4, H = g_info.draw_lines;
    uint32_t w, h;
    const uint32_t H0 = g_pt_h0, H1 = g_pt_h1 ? g_pt_h1 : H, hr = H1 - H0 + 1;
    if (kind == 0) w = 1 + pt_rand(s) % 64, h = H0 + pt_rand(s) % hr;        // small (a clock digit, a value)
    else if (kind == 1) w = 1 + pt_rand(s) % SCREEN_W, h = H0 + pt_rand(s) % hr;
    else if (kind == 2) w = 1 + pt_rand(s) % SCREEN_W, h = H1;               // the tallest allowed
    else w = SCREEN_W, h = H0 + pt_rand(s) % hr;                             // full width
    const int x1 = pt_rand(s) % (SCREEN_W - w + 1), y1 = pt_rand(s) % (SCREEN_H - h + 1);
    for (size_t i = 0; i < (size_t)w * h; i++) src[i] = (uint16_t)(pt_rand(s) | 1);

    ppa_srm_oper_config_t op;
    ppa_op(op, x1, y1, w, h, src, fb);
    op.mode = PPA_TRANS_MODE_NON_BLOCKING;
    static volatile bool fin;   // static: a late answer after a hang must not land on a stack frame
    fin = false;
    op.user_data = (void *)&fin;
    const uint32_t t0 = micros();
    if (ppa_do_scale_rotate_mirror(cl, &op) != ESP_OK) {
      err++;
      if (listed++ < 8) PT_SAY("%d %d %lux%lu: refused by the driver\n", x1, y1, (unsigned long)w, (unsigned long)h);
      continue;
    }
    while (!fin && micros() - t0 < 500000) delayMicroseconds(50);
    if (!fin) {
      hung = true;
      PT_SAY("%d %d %lux%lu: NO ANSWER after 500 ms, test stopped\n", x1, y1, (unsigned long)w, (unsigned long)h);
      break;
    }
    us += micros() - t0;

    // the panel rows this rectangle can reach (+-2 for neighbours), read fresh from memory
    int ax, ay, bx, by;
    pt_map(x1, y1, ax, ay);
    pt_map(x1 + w - 1, y1 + h - 1, bx, by);
    const int r0 = max(0, min(ay, by) - 2), r1 = min(PANEL_H - 1, max(ay, by) + 2);
    pt_sync(fb, (size_t)r0 * PANEL_W, (size_t)(r1 + 1) * PANEL_W, align, ESP_CACHE_MSYNC_FLAG_DIR_M2C);
    const int c0 = min(ax, bx), c1 = max(ax, bx);
    uint32_t in_bad = 0, out_bad = 0;
    int fx = -1, fy = -1;
    uint16_t got = 0, want = 0;
    for (int py = r0; py <= r1; py++)
      for (int px = 0; px < PANEL_W; px++) {
        const uint16_t v = fb[(size_t)py * PANEL_W + px];
        uint16_t e = 0;
        const bool inside = px >= c0 && px <= c1 && py >= min(ay, by) && py <= max(ay, by);
        if (inside) {   // invert pt_map: which picture pixel lands here
#if PANEL_ROT == 1
          const int x = py, y = PANEL_W - 1 - px;
#else
          const int x = PANEL_H - 1 - py, y = px;
#endif
          e = src[(size_t)(y - y1) * w + (x - x1)];
        }
        if (v != e) {
          inside ? in_bad++ : out_bad++;
          if (fx < 0) fx = px, fy = py, got = v, want = e;
        }
      }
    // back to 0 for the next rectangle
    memset(fb + (size_t)r0 * PANEL_W, 0, (size_t)(r1 - r0 + 1) * PANEL_W * 2);
    pt_sync(fb, (size_t)r0 * PANEL_W, (size_t)(r1 + 1) * PANEL_W, align, ESP_CACHE_MSYNC_FLAG_DIR_C2M);
    if (!in_bad && !out_bad) {
      ok++;
      continue;
    }
    bad++;
    if (listed++ < 8)
      PT_SAY("%d %d %lux%lu: %lu wrong inside, %lu outside; first panel %d,%d got %04x want %04x\n", x1, y1,
             (unsigned long)w, (unsigned long)h, (unsigned long)in_bad, (unsigned long)out_bad, fx, fy, got, want);
  }
  if (!hung) ppa_unregister_client(cl);
  else if (g_ppa) {   // the engine may be stuck: keep the screen alive on the CPU path
    g_ppa = NULL;
    g_disp_mode = "rotated by the CPU (PPA test: a copy never finished)";
  }
  free(src);
  char sum[160];
  snprintf(sum, sizeof sum, "done %d: ok %lu, wrong %lu, refused %lu%s; %lu us per copy\n", done, (unsigned long)ok,
           (unsigned long)bad, (unsigned long)err, hung ? ", HUNG" : "", (unsigned long)(ok + bad ? us / (ok + bad) : 0));
  // summary right after the first line
  const size_t sl = strlen(sum);
  if (n + sl < cap) {
    memmove(out + head + sl, out + head, n - head);
    memcpy(out + head, sum, sl);
    n += sl;
    out[n] = 0;
  }
  return n;
#undef PT_SAY
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

const char *board_display_mode(void) {
  static char buf[120];
  snprintf(buf, sizeof buf, "%s; %lu thin or unaligned rectangles by the CPU", g_disp_mode, (unsigned long)g_cpu_flushes);
  return buf;
}

// the P4's flash controller (SPI0) names its registers SPI_MEM_C_*
const char *board_flash_mode(uint32_t *ctrl) {
  const uint32_t c = REG_READ(SPI_MEM_C_CTRL_REG);
  if (ctrl) *ctrl = c;
  return c & SPI_MEM_C_FREAD_QIO ? "QIO" : c & SPI_MEM_C_FREAD_QUAD ? "QOUT" : c & SPI_MEM_C_FREAD_DIO ? "DIO" :
         c & SPI_MEM_C_FREAD_DUAL ? "DOUT" : "single";
}
