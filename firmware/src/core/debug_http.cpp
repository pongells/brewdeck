// Debug web server (see debug_http.h), only in debug builds (-DBREWDECK_DEBUG_HTTP: no open port in release builds).
// Screenshots and the recorder read the panel's frame buffer (board_screen_rows), i.e. exactly what is on the glass.
// While a download runs (~1-3 s) the extra PSRAM reads may disturb the picture briefly: debug use only.
#include <Arduino.h>
#ifdef BREWDECK_DEBUG_HTTP
#include <WiFi.h>
#include <esp_flash.h>
#include <esp_heap_caps.h>
#include <string.h>
#include "debug_http.h"
#include "board_core.h"
#include "net.h"
#include "tlog.h"

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

static void send_bmp(WiFiClient &c) {
  const uint32_t row = (int)bd_display()->width * 2, img = row * (int)bd_display()->height, off = 14 + 40 + 12;
  uint8_t h[off];
  memset(h, 0, sizeof h);
  h[0] = 'B'; h[1] = 'M';
  put32(h + 2, off + img);
  put32(h + 10, off);
  put32(h + 14, 40);                                // BITMAPINFOHEADER
  put32(h + 18, (int)bd_display()->width);
  put32(h + 22, (uint32_t)(-(int32_t)(int)bd_display()->height));    // negative height: rows top-down, as in the frame buffer
  put16(h + 26, 1);
  put16(h + 28, 16);
  put32(h + 30, 3);                                 // BI_BITFIELDS: RGB565 masks follow
  put32(h + 34, img);
  put32(h + 54, 0xF800); put32(h + 58, 0x07E0); put32(h + 62, 0x001F);
  c.printf("HTTP/1.0 200 OK\r\nContent-Type: image/bmp\r\nContent-Length: %u\r\nCache-Control: no-store\r\n"
           "Connection: close\r\n\r\n", (unsigned)(off + img));
  c.write(h, sizeof h);
  // from the board's frame buffer in PSRAM (S3: straight from it; lwIP copies into its own buffers): no internal-RAM
  // staging buffer
  const size_t CH = (int)bd_display()->width * 2 * 8;               // 8 rows per write
  for (int y = 0; y < (int)bd_display()->height; y += 8) {
    const uint8_t *chunk = (const uint8_t *)board_screen_rows(y, 8);
    if (!chunk) return;
    size_t sent = 0;
    uint32_t t0 = millis();
    while (sent < CH && c.connected() && millis() - t0 < 5000) {
      size_t n = c.write(chunk + sent, CH - sent);
      if (n == 0) delay(2); else sent += n;
    }
    if (sent < CH) return;
  }
}

// Recorder (GET /rec?every=ms): copies what the panel shows into PSRAM every N ms, run-length coded, so a whole shot can
// be recorded without sending anything over Wi-Fi meanwhile (a 768 KB screenshot takes 3-5 s on a weak link and competes
// with the machine's data); GET /rec.bin downloads the lot afterwards (tools/record.sh turns it into PNGs).
// Format, per frame: "BDRF", u32 ms since the start, u32 payload bytes, u16 width, u16 height, then (u16 count,
// u16 RGB565 pixel) pairs, little endian. Taken on this task, not the UI's: a frame may be caught mid-redraw (torn).
static uint8_t *g_rec = NULL;
static size_t g_rec_cap = 0, g_rec_len = 0;
static int g_rec_n = 0, g_rec_max = 0;
static uint32_t g_rec_every = 0, g_rec_t0 = 0, g_rec_next = 0, g_rec_us = 0;
static volatile bool g_rec_on = false;

static void rec_stop(const char *why) {
  if (!g_rec_on) return;
  g_rec_on = false;
  Log.printf("rec: stopped (%s): %d frames, %u KB, last copy %u ms\n", why, g_rec_n, (unsigned)(g_rec_len / 1024),
             (unsigned)(g_rec_us / 1000));
}
static bool rec_start(uint32_t every, int max) {
  rec_stop("restarted");
  if (!g_rec) {   // leave PSRAM for the UI: at most 16 MB, and 4 MB (or half, on a small PSRAM) stays free
    const size_t fr = heap_caps_get_free_size(MALLOC_CAP_SPIRAM), lg = heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM);
    size_t cap = fr > 8u << 20 ? fr - (4u << 20) : fr / 2;
    if (cap > 16u << 20) cap = 16u << 20;
    if (cap > lg) cap = lg;
    g_rec = cap >= 1u << 20 ? (uint8_t *)heap_caps_malloc(cap, MALLOC_CAP_SPIRAM) : NULL;
    g_rec_cap = g_rec ? cap : 0;
  }
  if (!g_rec) return false;
  g_rec_len = 0; g_rec_n = 0; g_rec_us = 0;
  g_rec_every = every < 200 ? 200 : every;
  g_rec_max = max;
  g_rec_t0 = g_rec_next = millis();
  g_rec_on = true;
  Log.printf("rec: started, every %u ms, %u KB of PSRAM\n", (unsigned)g_rec_every, (unsigned)(g_rec_cap / 1024));
  return true;
}
static void rec_capture(void) {
  const uint32_t t0 = micros(), now = millis();
  const size_t HDR = 16;
  uint8_t *const base = g_rec + g_rec_len, *out = base + HDR, *const end = g_rec + g_rec_cap;
  if (out >= end) { rec_stop("memory full"); return; }
  uint16_t cur = 0, run = 0;
  for (int y = 0; y < (int)bd_display()->height; y += 8) {
    const uint16_t *px = board_screen_rows(y, 8);
    if (!px) { rec_stop("no frame buffer"); return; }
    for (int i = 0; i < (int)bd_display()->width * 8; i++) {
      if (run && px[i] == cur && run < 0xFFFF) { run++; continue; }
      if (run) {
        if (out + 4 > end) { rec_stop("memory full"); return; }
        put16(out, run); put16(out + 2, cur); out += 4;
      }
      cur = px[i]; run = 1;
    }
  }
  if (out + 4 > end) { rec_stop("memory full"); return; }
  put16(out, run); put16(out + 2, cur); out += 4;
  memcpy(base, "BDRF", 4);
  put32(base + 4, now - g_rec_t0);
  put32(base + 8, (uint32_t)(out - base - HDR));
  put16(base + 12, (int)bd_display()->width); put16(base + 14, (int)bd_display()->height);
  g_rec_len = out - g_rec;
  g_rec_n++;
  g_rec_us = micros() - t0;
  if (g_rec_n % 30 == 0)
    Log.printf("rec: %d frames, %u KB, last copy %u ms\n", g_rec_n, (unsigned)(g_rec_len / 1024), (unsigned)(g_rec_us / 1000));
  if (g_rec_max && g_rec_n >= g_rec_max) rec_stop("frame limit");
}
static void rec_step(void) {
  if (!g_rec_on || (int32_t)(millis() - g_rec_next) < 0) return;
  g_rec_next += g_rec_every;
  if ((int32_t)(millis() - g_rec_next) >= 0) g_rec_next = millis() + g_rec_every;   // fell behind (a download ran)
  rec_capture();
}
static void send_rec(WiFiClient &c) {
  rec_stop("download");
  c.printf("HTTP/1.0 200 OK\r\nContent-Type: application/octet-stream\r\nContent-Length: %u\r\nCache-Control: no-store\r\n"
           "Connection: close\r\n\r\n", (unsigned)g_rec_len);
  size_t sent = 0;
  uint32_t t0 = millis();
  while (sent < g_rec_len && c.connected() && millis() - t0 < 10000) {
    const size_t want = g_rec_len - sent < 8192 ? g_rec_len - sent : 8192;
    const size_t n = c.write(g_rec + sent, want);
    if (n == 0) delay(2); else { sent += n; t0 = millis(); }
  }
  Log.printf("rec: downloaded %u of %u bytes\n", (unsigned)sent, (unsigned)g_rec_len);
}

static void task(void *) {
  char ip[16] = "", machine[64];
  while (!net_wifi_up(NULL)) delay(500);
  net_addr_info(ip, sizeof ip, machine, sizeof machine);
  g_srv.begin();
  Log.printf("screenshots: http://%s.local/screen.bmp (http://%s/screen.bmp)\n", board_hostname(), ip);
  for (;;) {
    WiFiClient c = g_srv.available();
    if (!c) { rec_step(); delay(20); continue; }
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
    if (strncmp(line, "GET /screen.bmp ", 16) == 0 && board_screen_rows(0, 1)) {
      uint32_t t0 = millis();
      send_bmp(c);
      mem_watch("screenshot");
      Log.printf("screenshot sent in %u ms\n", (unsigned)(millis() - t0));
    } else if (strncmp(line, "GET /rec?", 9) == 0) {   // recorder: /rec?every=2000[&max=frames]
      const char *pe = strstr(line, "every="), *pm = strstr(line, "max=");
      const bool ok = rec_start(pe ? (uint32_t)atoi(pe + 6) : 2000, pm ? atoi(pm + 4) : 0);
      c.printf("HTTP/1.0 200 OK\r\nContent-Type: text/plain\r\nConnection: close\r\n\r\n%s\n",
               ok ? "recording" : "no PSRAM for the recorder");
    } else if (strncmp(line, "GET /rec ", 9) == 0 || strncmp(line, "GET /rec/stop ", 14) == 0) {   // status / stop
      if (line[8] == '/') rec_stop("asked");
      c.printf("HTTP/1.0 200 OK\r\nContent-Type: text/plain\r\nConnection: close\r\n\r\n"
               "%s, %d frames, %u of %u KB, every %u ms, last copy %u ms\n", g_rec_on ? "recording" : "stopped", g_rec_n,
               (unsigned)(g_rec_len / 1024), (unsigned)(g_rec_cap / 1024), (unsigned)g_rec_every, (unsigned)(g_rec_us / 1000));
    } else if (strncmp(line, "GET /rec.bin ", 13) == 0 && g_rec) {
      send_rec(c);
    } else if (strncmp(line, "GET /rec/free ", 14) == 0) {   // give the PSRAM back
      rec_stop("freed");
      heap_caps_free(g_rec);
      g_rec = NULL; g_rec_cap = g_rec_len = 0; g_rec_n = 0;
      c.print("HTTP/1.0 200 OK\r\nContent-Type: text/plain\r\nConnection: close\r\n\r\nfreed\n");
    } else if (strncmp(line, "GET /heap ", 10) == 0) {   // memory and Wi-Fi link
      char ssid[33], ap[18];
      int8_t rssi;
      int ch;
      net_wifi_info(ssid, sizeof ssid, &rssi, &ch, ap, sizeof ap);
      c.printf("HTTP/1.0 200 OK\r\nContent-Type: text/plain\r\nConnection: close\r\n\r\n"
               "internal free %u, largest block %u, lowest since boot %u\npsram free %u\nwifi rssi %d dBm, channel %d, ap %s\n",
               (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
               (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL),
               (unsigned)heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL),
               (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM), (int)rssi, ch, ap);
    } else if (strncmp(line, "GET /flash ", 11) == 0) {   // flash read mode + raw read speed (debug)
      uint32_t ctrl = 0;
      const char *mode = board_flash_mode(&ctrl);   // chip-specific registers: the board layer reads them
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
      if (pp) bd_set_backlight(atoi(pp + 4));
      c.print("HTTP/1.0 200 OK\r\nContent-Type: text/plain\r\nConnection: close\r\n\r\nok\n");
    } else {
      c.print("HTTP/1.0 404 Not Found\r\nContent-Type: text/plain\r\nConnection: close\r\n\r\nGET /screen.bmp, /rec?every=ms[&max=n], /rec, /rec/stop, /rec.bin, /rec/free, /heap, /tasks, /flash, /roam, /touch?x=&y=&ms=, /bl?pct=&hz=\n");
    }
    c.stop();
  }
}

void debug_http_begin(void) {
  xTaskCreatePinnedToCore(task, "debug_http", 4096, NULL, 1, NULL, 0);
}
void debug_http_route(const char *path, bd_debug_fn fn) {
  snprintf(g_rt_path, sizeof g_rt_path, "%s", path ? path : "");
  g_rt_fn = fn;
}
void debug_http_service(void) {
  if (!g_rt_req) return;
  g_rt_req = false;
  const size_t n = g_rt_fn ? g_rt_fn(g_rt_out, sizeof g_rt_out) : 0;
  g_rt_out[n < sizeof g_rt_out ? n : sizeof g_rt_out - 1] = 0;
  g_rt_done = true;
}
#endif  // BREWDECK_DEBUG_HTTP
