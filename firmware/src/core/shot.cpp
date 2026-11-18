// Debug web server (see shot.h), only in debug builds (env ui-debug, -DBREWDECK_DEBUG_HTTP: no
// open port in release builds). Screenshot server: reads the RGB panel's frame buffer in PSRAM, i.e. exactly what is on the glass.
// While a download runs (~1-3 s) the extra PSRAM reads may disturb the picture briefly: debug use only.
#include <Arduino.h>
#ifdef BREWDECK_DEBUG_HTTP
#include "tlog.h"
#include <WiFi.h>
#include <string.h>
#include <esp_heap_caps.h>
#include <esp_flash.h>
#include "soc/spi_mem_reg.h"
#include "board.h"
#include "net.h"
#include "shot.h"
#include "../board/board_internal.h"

static WiFiServer g_srv(80);
// the UI's own debug route (bd_debug_route)
static char g_rt_path[24] = "";
static bd_debug_fn g_rt_fn = NULL;
static volatile bool g_rt_req = false, g_rt_done = false;
static char g_rt_out[1024];
static bool route_match(const char *line) {      // "GET <path> " or "GET <path>?..."
  const size_t n = strlen(g_rt_path);
  return n && g_rt_fn && !strncmp(line, "GET ", 4) && !strncmp(line + 4, g_rt_path, n) &&
         (line[4 + n] == ' ' || line[4 + n] == '?');
}

static void put16(uint8_t *p, uint16_t v) { p[0] = v; p[1] = v >> 8; }
static void put32(uint8_t *p, uint32_t v) { for (int i = 0; i < 4; i++) p[i] = v >> (8 * i); }

static void send_bmp(WiFiClient &c, const uint16_t *fb) {
  const uint32_t row = SCREEN_W * 2, img = row * SCREEN_H, off = 14 + 40 + 12;
  uint8_t h[off];
  memset(h, 0, sizeof h);
  h[0] = 'B'; h[1] = 'M';
  put32(h + 2, off + img);
  put32(h + 10, off);
  put32(h + 14, 40);                                // BITMAPINFOHEADER
  put32(h + 18, SCREEN_W);
  put32(h + 22, (uint32_t)(-(int32_t)SCREEN_H));    // negative height: rows top-down, as in the frame buffer
  put16(h + 26, 1);
  put16(h + 28, 16);
  put32(h + 30, 3);                                 // BI_BITFIELDS: RGB565 masks follow
  put32(h + 34, img);
  put32(h + 54, 0xF800); put32(h + 58, 0x07E0); put32(h + 62, 0x001F);
  c.printf("HTTP/1.0 200 OK\r\nContent-Type: image/bmp\r\nContent-Length: %u\r\nCache-Control: no-store\r\n"
           "Connection: close\r\n\r\n", (unsigned)(off + img));
  c.write(h, sizeof h);
  // straight from the frame buffer in PSRAM (lwIP copies into its own buffers): no internal-RAM staging buffer
  const size_t CH = SCREEN_W * 2 * 8;               // 8 rows per write
  for (int y = 0; y < SCREEN_H; y += 8) {
    const uint8_t *chunk = (const uint8_t *)(fb + (size_t)y * SCREEN_W);
    size_t sent = 0;
    uint32_t t0 = millis();
    while (sent < CH && c.connected() && millis() - t0 < 5000) {
      size_t n = c.write(chunk + sent, CH - sent);
      if (n == 0) delay(2); else sent += n;
    }
    if (sent < CH) return;
  }
}

static void task(void *) {
  while (WiFi.status() != WL_CONNECTED) delay(500);
  g_srv.begin();
  Log.printf("screenshots: http://brewdeck.local/screen.bmp (http://%s/screen.bmp)\n", WiFi.localIP().toString().c_str());
  for (;;) {
    WiFiClient c = g_srv.available();
    if (!c) { delay(50); continue; }
    c.setTimeout(2000);   // milliseconds (Stream::setTimeout), not seconds
    c.setNoDelay(true);   // no Nagle: with the small lwIP send buffer it stalls on the receiver's delayed ACKs
    char line[96] = "";
    size_t n = c.readBytesUntil('\n', line, sizeof line - 1);
    line[n] = 0;
    while (c.connected()) {                         // skip the request headers
      char hb[128];
      size_t m = c.readBytesUntil('\n', hb, sizeof hb - 1);
      if (m <= 1) break;
    }
    uint16_t *fb = gfx ? gfx->getFramebuffer() : NULL;
    if (strncmp(line, "GET /screen.bmp ", 16) == 0 && fb) {
      uint32_t t0 = millis();
      send_bmp(c, fb);
      mem_watch("screenshot");
      Log.printf("screenshot sent in %u ms\n", (unsigned)(millis() - t0));
    } else if (strncmp(line, "GET /heap ", 10) == 0) {   // memory check (debug)
      c.printf("HTTP/1.0 200 OK\r\nContent-Type: text/plain\r\nConnection: close\r\n\r\n"
               "internal free %u, largest block %u, lowest since boot %u\npsram free %u\nwifi rssi %d dBm, channel %d, ap %s\n",
               (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
               (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL),
               (unsigned)heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL),
               (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM), (int)WiFi.RSSI(), (int)WiFi.channel(),
               WiFi.BSSIDstr().c_str());
    } else if (strncmp(line, "GET /flash ", 11) == 0) {   // flash read mode + raw read speed (debug)
      const uint32_t ctrl = REG_READ(SPI_MEM_CTRL_REG(0));
      const char *mode = ctrl & SPI_MEM_FREAD_QIO ? "QIO" : ctrl & SPI_MEM_FREAD_QUAD ? "QOUT" :
                         ctrl & SPI_MEM_FREAD_DIO ? "DIO" : ctrl & SPI_MEM_FREAD_DUAL ? "DOUT" : "single";
      static uint8_t *buf;
      if (!buf) buf = (uint8_t *)heap_caps_malloc(4096, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
      const uint32_t N = 1024 * 1024, t0 = micros();
      for (uint32_t o = 0; buf && o < N; o += 4096) esp_flash_read(NULL, buf, 0x10000 + o, 4096);   // raw, app area
      const uint32_t us = micros() - t0;
      c.printf("HTTP/1.0 200 OK\r\nContent-Type: text/plain\r\nConnection: close\r\n\r\n"
               "flash read mode %s (SPI_MEM_CTRL 0x%08x)\n1 MB raw read in %u ms (%.1f MB/s)\n",
               mode, (unsigned)ctrl, (unsigned)(us / 1000), us ? 1e6f / us : 0.f);
    } else if (strncmp(line, "GET /tasks ", 11) == 0) {   // stack use per task (debug): unused bytes at worst
      static TaskStatus_t ts[24];
      uint32_t total;
      const UBaseType_t n = uxTaskGetSystemState(ts, 24, &total);
      c.print("HTTP/1.0 200 OK\r\nContent-Type: text/plain\r\nConnection: close\r\n\r\ntask            core  prio  stack never used (bytes)\n");
      for (UBaseType_t i = 0; i < n; i++)
        c.printf("%-16s %4d %5u %6u\n", ts[i].pcTaskName, ts[i].xCoreID == tskNO_AFFINITY ? -1 : (int)ts[i].xCoreID,
                 (unsigned)ts[i].uxCurrentPriority, (unsigned)ts[i].usStackHighWaterMark);
    } else if (strncmp(line, "GET /roam ", 10) == 0) {   // roaming test (debug): scan now, move if clearly better (log)
      net_roam_check();
      c.print("HTTP/1.0 200 OK\r\nContent-Type: text/plain\r\nConnection: close\r\n\r\nroaming check started, see the serial log\n");
    } else if (route_match(line)) {                  // the UI's route (bd_debug_route): run by the UI task
      g_rt_done = false;
      g_rt_req = true;
      const uint32_t t0 = millis();
      while (!g_rt_done && millis() - t0 < 30000) delay(20);
      c.printf("HTTP/1.0 200 OK\r\nContent-Type: text/plain\r\nConnection: close\r\n\r\n%s", g_rt_done ? g_rt_out : "timeout\n");
    } else if (strncmp(line, "GET /touch?", 11) == 0) {   // remote touch (debug): /touch?x=400&y=240&ms=100
      const char *px = strstr(line, "x="), *py = strstr(line, "y="), *pm = strstr(line, "ms=");
      if (px && py) board_fake_touch(atoi(px + 2), atoi(py + 2), pm ? atoi(pm + 3) : 100);
      c.print("HTTP/1.0 200 OK\r\nContent-Type: text/plain\r\nConnection: close\r\n\r\nok\n");
    } else if (strncmp(line, "GET /bl?", 8) == 0) {   // backlight test (debug): /bl?pct=50&hz=1000
      const char *ph = strstr(line, "hz="), *pp = strstr(line, "pct=");
      if (ph) board_backlight_hz((uint32_t)atoi(ph + 3));
      if (pp) board_backlight(atoi(pp + 4));
      c.print("HTTP/1.0 200 OK\r\nContent-Type: text/plain\r\nConnection: close\r\n\r\nok\n");
    } else {
      c.print("HTTP/1.0 404 Not Found\r\nContent-Type: text/plain\r\nConnection: close\r\n\r\nGET /screen.bmp, /heap, /tasks, /flash, /roam, /touch?x=&y=&ms=, /bl?pct=&hz=\n");
    }
    c.stop();
  }
}

void shot_begin(void) {
  xTaskCreatePinnedToCore(task, "shot", 4096, NULL, 1, NULL, 0);
}
void shot_route(const char *path, bd_debug_fn fn) {
  snprintf(g_rt_path, sizeof g_rt_path, "%s", path ? path : "");
  g_rt_fn = fn;
}
void shot_service(void) {
  if (!g_rt_req) return;
  g_rt_req = false;
  const size_t n = g_rt_fn ? g_rt_fn(g_rt_out, sizeof g_rt_out) : 0;
  g_rt_out[n < sizeof g_rt_out ? n : sizeof g_rt_out - 1] = 0;
  g_rt_done = true;
}
#endif  // BREWDECK_DEBUG_HTTP
