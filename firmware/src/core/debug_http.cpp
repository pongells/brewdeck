// Debug web server (see debug_http.h), only in debug builds (-DBREWDECK_DEBUG_HTTP: no open port in release builds).
// Screenshots and the recorder read the panel's frame buffer (board_screen_rows), i.e. exactly what is on the glass.
// While a download runs (~1-3 s) the extra PSRAM reads may disturb the picture briefly: debug use only.
#include "systime.h"
#ifdef BREWDECK_DEBUG_HTTP
#include "tcp.h"
#include <esp_flash.h>
#include <esp_ota_ops.h>
#include <esp_partition.h>
#include <esp_heap_caps.h>
#include <esp_system.h>
#include <string.h>
#include "debug_http.h"
#include "board_core.h"
#include "net.h"
#include "tlog.h"
#include "logsend.h"
#include "ota.h"
#include "update.h"
#if __has_include("brewdeck_version.h")
#include "brewdeck_version.h"
#endif
#ifndef BREWDECK_VERSION
#define BREWDECK_VERSION ""
#endif
#include "frames.h"

static TcpListener g_srv;
// the UI's own debug route (bd_debug_route)
static char g_rt_path[24] = "";
static bd_debug_fn g_rt_fn = NULL;
// a job for the UI task (the UI's route, the I2C scan): the server task waits for its text
static bd_debug_fn volatile g_rt_job = NULL;
static volatile bool g_rt_done = false;
static char g_rt_out[1024];
static bool route_match(const char *line) {      // "GET <path> " or "GET <path>?..."
  const size_t n = strlen(g_rt_path);
  return n && g_rt_fn && !strncmp(line, "GET ", 4) && !strncmp(line + 4, g_rt_path, n) &&
         (line[4 + n] == ' ' || line[4 + n] == '?');
}

// Big downloads (screenshot, recording, memory log) send faster than Wi-Fi drains, and the frames waiting to go out
// sit in internal RAM, which Wi-Fi itself needs (seen on the S3: down to 3.6 KB after 3 screenshots). Before each
// chunk (small: one write of 8 rows took ~10 KB at once), wait (up to 1 s) while less than this is free.
static const size_t KEEP_FREE = 24 * 1024;
static void wait_ram(void) {
  for (int i = 0; i < 500 && heap_caps_get_free_size(MALLOC_CAP_INTERNAL) < KEEP_FREE; i++) wait_ms(2);
}

static void put16(uint8_t *p, uint16_t v) { p[0] = v; p[1] = v >> 8; }
static void put32(uint8_t *p, uint32_t v) { for (int i = 0; i < 4; i++) p[i] = v >> (8 * i); }

static void send_bmp(TcpConn &c) {
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
  const int ROWS = 2;                                               // rows per write
  const size_t CH = (int)bd_display()->width * 2 * ROWS;
  for (int y = 0; y < (int)bd_display()->height; y += ROWS) {
    wait_ram();
    const uint8_t *chunk = (const uint8_t *)board_screen_rows(y, ROWS);
    if (!chunk) return;
    size_t sent = 0;
    uint32_t t0 = clock_ms();
    while (sent < CH && c.connected() && clock_ms() - t0 < 5000) {
      size_t n = c.write(chunk + sent, CH - sent);
      if (n == 0) wait_ms(2); else sent += n;
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
  g_rec_t0 = g_rec_next = clock_ms();
  g_rec_on = true;
  Log.printf("rec: started, every %u ms, %u KB of PSRAM\n", (unsigned)g_rec_every, (unsigned)(g_rec_cap / 1024));
  return true;
}
static void rec_capture(void) {
  const uint32_t t0 = clock_us(), now = clock_ms();
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
  g_rec_us = clock_us() - t0;
  if (g_rec_n % 30 == 0)
    Log.printf("rec: %d frames, %u KB, last copy %u ms\n", g_rec_n, (unsigned)(g_rec_len / 1024), (unsigned)(g_rec_us / 1000));
  if (g_rec_max && g_rec_n >= g_rec_max) rec_stop("frame limit");
}
static void rec_step(void) {
  if (!g_rec_on || (int32_t)(clock_ms() - g_rec_next) < 0) return;
  g_rec_next += g_rec_every;
  if ((int32_t)(clock_ms() - g_rec_next) >= 0) g_rec_next = clock_ms() + g_rec_every;   // fell behind (a download ran)
  rec_capture();
}
static void send_rec(TcpConn &c) {
  rec_stop("download");
  c.printf("HTTP/1.0 200 OK\r\nContent-Type: application/octet-stream\r\nContent-Length: %u\r\nCache-Control: no-store\r\n"
           "Connection: close\r\n\r\n", (unsigned)g_rec_len);
  size_t sent = 0;
  uint32_t t0 = clock_ms();
  while (sent < g_rec_len && c.connected() && clock_ms() - t0 < 10000) {
    wait_ram();
    const size_t want = g_rec_len - sent < 4096 ? g_rec_len - sent : 4096;
    const size_t n = c.write(g_rec + sent, want);
    if (n == 0) wait_ms(2); else { sent += n; t0 = clock_ms(); }
  }
  Log.printf("rec: downloaded %u of %u bytes\n", (unsigned)sent, (unsigned)g_rec_len);
}

static void task(void *) {
  char ip[16] = "", machine[64];
  while (!net_wifi_up(NULL)) wait_ms(500);
  net_addr_info(ip, sizeof ip, machine, sizeof machine);
  while (!g_srv.begin(80)) wait_ms(1000);
  Log.printf("screenshots: http://%s.local/screen.bmp (http://%s/screen.bmp)\n", board_hostname(), ip);
  for (;;) {
    const int fd = g_srv.accept(20);                // waits up to 20 ms for a request
    if (fd < 0) { rec_step(); continue; }
    TcpConn c(fd);
    c.set_timeout(2000);
    c.set_nodelay(true);  // no Nagle: with the small lwIP send buffer it stalls on the receiver's delayed ACKs
    char line[96];
    c.read_line(line, sizeof line);
    size_t clen = 0;                                // request headers: only these two are used (POST /ota)
    char token[48] = "";
    while (c.connected()) {
      char hb[128];
      const size_t m = c.read_line(hb, sizeof hb);
      if (m <= 1) break;
      if (hb[m - 1] == '\r') hb[m - 1] = 0;
      if (!strncasecmp(hb, "Content-Length:", 15)) clen = (size_t)strtoul(hb + 15, NULL, 10);
      else if (!strncasecmp(hb, "X-OTA-Token:", 12)) {
        const char *v = hb + 12;
        while (*v == ' ') v++;
        snprintf(token, sizeof token, "%s", v);
      }
    }
    if (strncmp(line, "POST /ota ", 10) == 0) {   // firmware pushed by build.sh --ota (ota.h)
      static char msg[200];
      c.set_timeout(10000);
      const bool ok = ota_receive(c, clen, token, msg, sizeof msg);
      c.printf("HTTP/1.0 %s\r\nContent-Type: text/plain\r\nConnection: close\r\n\r\n%s", ok ? "200 OK" : "409 Conflict", msg);
      c.close();
      if (ok) ota_restart();
      continue;
    } else if (strncmp(line, "GET /screen.bmp ", 16) == 0 && board_screen_rows(0, 1)) {
      uint32_t t0 = clock_ms();
      send_bmp(c);
      mem_watch("screenshot");
      Log.printf("screenshot sent in %u ms\n", (unsigned)(clock_ms() - t0));
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
    } else if (strncmp(line, "GET /log/prev ", 14) == 0) {   // the log tail from before a crash / watchdog restart
      static char prev[3072];
      const size_t n = log_before_restart(prev, sizeof prev);
      c.print("HTTP/1.0 200 OK\r\nContent-Type: text/plain\r\nConnection: close\r\n\r\n");
      if (n) c.write((const uint8_t *)prev, n);
      else c.print("(nothing kept: the last start was a power-on, a flash or a reset by USB)\n");
    } else if (strncmp(line, "GET /log", 8) == 0 && (line[8] == ' ' || line[8] == '?')) {
      // the memory log (last 256 KB); /log?from=N continues after an earlier read (X-Log-Next)
      const char *pf = strstr(line, "from=");
      uint32_t pos = pf ? (uint32_t)strtoul(pf + 5, NULL, 10) : 0;
      const uint32_t end = log_written();
      c.printf("HTTP/1.0 200 OK\r\nContent-Type: text/plain\r\nX-Log-Next: %lu\r\nConnection: close\r\n\r\n",
               (unsigned long)end);
      static char chunk[1460];
      while (pos < end && c.connected()) {
        wait_ram();
        uint32_t next;
        const size_t n = log_read(pos, chunk, sizeof chunk, &next);
        if (!n) break;
        if (next > end) {   // stop at the end seen in the header
          c.write((const uint8_t *)chunk, n - (next - end));
          break;
        }
        c.write((const uint8_t *)chunk, n);
        pos = next;
      }
    } else if (strncmp(line, "GET /loghost", 12) == 0 && (line[12] == ' ' || line[12] == '?')) {
      const char *pt = strstr(line, "to=");      // /loghost?to=192.168.1.10[:5140] sets it, /loghost?to= turns it off
      if (pt) {
        char to[64];
        size_t k = 0;
        for (pt += 3; *pt && *pt != ' ' && *pt != '&' && k < sizeof to - 1; pt++) to[k++] = *pt;
        to[k] = 0;
        logsend_set_target(to);
      }
      const char *t = logsend_target();
      uint32_t sent, err, unres;
      logsend_counts(&sent, &err, &unres);
      c.printf("HTTP/1.0 200 OK\r\nContent-Type: text/plain\r\nConnection: close\r\n\r\nlog server: %s\n"
               "lines sent %lu, send errors %lu, address not found %lu\n", t[0] ? t : "off", (unsigned long)sent,
               (unsigned long)err, (unsigned long)unres);
    } else if (strncmp(line, "GET /release", 12) == 0 && (line[12] == ' ' || line[12] == '?')) {
      // /release shows the release state; ?base=http://host:8000/dir takes releases from there (test key accepted),
      // ?base= goes back to GitHub; ?check=1 looks now; ?install=1 installs the newer release (as the UI would)
      const char *pb = strstr(line, "base=");
      if (pb) {
        char base[96];
        size_t k = 0;
        for (pb += 5; *pb && *pb != ' ' && *pb != '&' && k < sizeof base - 1; pb++) base[k++] = *pb;
        base[k] = 0;
        update_set_base(base);
      } else if (strstr(line, "check=1")) {
        bd_release_check();
      } else if (const char *pi = strstr(line, "install=")) {   // 1: as the UI's Install button; <name>: that UI
        static const char *const RES[] = {"ok", "offline", "busy", "range", "profile changed", "no scale", "pending",
                                          "not in the catalogue / no newer release"};
        char name[16];
        size_t k = 0;
        for (pi += 8; *pi && *pi != ' ' && *pi != '&' && k < sizeof name - 1; pi++) name[k++] = *pi;
        name[k] = 0;
        const bd_result res = !strcmp(name, "1") ? bd_release_install() : bd_ui_install(name);
        c.printf("HTTP/1.0 200 OK\r\nContent-Type: text/plain\r\nConnection: close\r\n\r\ninstall: %s\n", RES[res]);
        c.close();
        continue;
      }
      static const char *const ST[] = {"unknown", "checking", "up to date", "update available", "check failed"};
      const bd_release *r = bd_release_info();
      c.printf("HTTP/1.0 200 OK\r\nContent-Type: text/plain\r\nConnection: close\r\n\r\nreleases from: %s\n"
               "state: %s, newest %s (%u bytes)%s%s%s\nrunning: %s\n", update_base()[0] ? update_base() : "GitHub",
               ST[r->state], r->version[0] ? r->version : "-", (unsigned)r->size, r->installing ? ", installing" : "",
               r->error[0] ? ", last error: " : "", r->error, ota_slot());
      const bd_catalog *cat = bd_ui_catalog();
      for (int i = 0; i < cat->n; i++)
        c.printf("interface %s \"%s\" (%u bytes)%s\n", cat->ui[i].name, cat->ui[i].title, (unsigned)cat->ui[i].size,
                 cat->ui[i].current ? ", running" : "");
    } else if (strncmp(line, "GET /heap ", 10) == 0) {   // memory and Wi-Fi link
      char ssid[33], ap[18];
      int8_t rssi;
      int ch;
      net_wifi_info(ssid, sizeof ssid, &rssi, &ch, ap, sizeof ap);
      c.printf("HTTP/1.0 200 OK\r\nContent-Type: text/plain\r\nConnection: close\r\n\r\n"
               "internal free %u, largest block %u, lowest since boot %u\npsram free %u\nwifi rssi %d dBm, channel %d, ap %s\n"
               "uptime %lu s, reset reason %d (esp_reset_reason)\ndisplay: %s\nfirmware: %s, built %s %s, %s\n",
               (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
               (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL),
               (unsigned)heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL),
               (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM), (int)rssi, ch, ap, (unsigned long)(clock_ms() / 1000),
               (int)esp_reset_reason(), board_display_mode(), BREWDECK_VERSION[0] ? BREWDECK_VERSION : "?", __DATE__,
               __TIME__, ota_slot());
    } else if (strncmp(line, "GET /frames ", 12) == 0) {   // frame times per shot / steam session (tools/frames.sh)
      static char rep[1600];
      frames_report(rep, sizeof rep);
      c.print("HTTP/1.0 200 OK\r\nContent-Type: text/plain\r\nConnection: close\r\n\r\n");
      c.print(rep);
    } else if (strncmp(line, "GET /flash ", 11) == 0) {   // flash read mode + raw read speed (debug)
      uint32_t ctrl = 0;
      const char *mode = board_flash_mode(&ctrl);   // chip-specific registers: the board layer reads them
      static uint8_t *buf;
      if (!buf) buf = (uint8_t *)heap_caps_malloc(4096, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
      const uint32_t N = 1024 * 1024, t0 = clock_us();
      for (uint32_t o = 0; buf && o < N; o += 4096) esp_flash_read(NULL, buf, 0x10000 + o, 4096);   // raw, app area
      const uint32_t us = clock_us() - t0;
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
    } else if (strncmp(line, "GET /flashtest?", 15) == 0) {   // debug: flash operations alone, on the free app slot
      // /flashtest?op=erase|write&ms=5000: 4 KB erases or 2 KB writes back to back, to see what the panel does
      const bool erase = strstr(line, "op=erase") != NULL;
      const char *pc = strstr(line, "chunk=");
      const size_t chunk = pc ? (size_t)atoi(pc + 6) : 2048;   // write size (1..2048)
      const bool pre = strstr(line, "pre=1") != NULL;          // erase 256 KB first, wait 5 s, then only writes
      const char *pm = strstr(line, "ms=");
      const uint32_t ms = pm ? (uint32_t)atoi(pm + 3) : 5000;
      const esp_partition_t *p = esp_ota_get_next_update_partition(NULL);
      static uint8_t wb[2048];
      memset(wb, 0xA5, sizeof wb);
      uint32_t n = 0, off = 0, worst = 0;
      const uint32_t t0 = clock_ms();
      if (p) {
        if (!erase) esp_partition_erase_range(p, 0, pre ? 256 * 1024 : 64 * 1024);   // something to write into
        if (pre) wait_ms(5000);
        const uint32_t t1 = clock_ms();
        while (clock_ms() - t1 < ms) {
          const uint32_t a0 = clock_us();
          if (erase) esp_partition_erase_range(p, off, 4096), off = (off + 4096) % (256 * 1024);
          else {
            const size_t cs = chunk < 1 ? 1 : chunk > sizeof wb ? sizeof wb : chunk;
            if (off + cs > (pre ? 256u : 64u) * 1024) {
              if (pre) break;                                  // pre-erased area used up: no erase in the test
              esp_partition_erase_range(p, 0, 64 * 1024);
              off = 0;
            }
            esp_partition_write(p, off, wb, cs), off += cs;
          }
          const uint32_t d = clock_us() - a0;
          if (d > worst) worst = d;
          n++;
          wait_ms(1);
        }
      }
      c.printf("HTTP/1.0 200 OK\r\nContent-Type: text/plain\r\nConnection: close\r\n\r\n%s: %lu in %lu ms, longest %lu us (%s)\n",
               erase ? "4 KB erases" : pre ? "writes into erased flash" : "2 KB writes", (unsigned long)n, (unsigned long)(clock_ms() - t0), (unsigned long)worst,
               p ? p->label : "no free slot");
    } else if (strncmp(line, "GET /ppatest?stall=1", 20) == 0) {   // the next PPA copy hangs: the fallback takes over
      board_ppa_stall();
      c.print("HTTP/1.0 200 OK\r\nContent-Type: text/plain\r\nConnection: close\r\n\r\nthe next PPA copy will hang\n");
    } else if (strncmp(line, "GET /ppatest", 12) == 0 && (line[12] == ' ' || line[12] == '?')) {
      const char *pn = strstr(line, "n="), *ps = strstr(line, "seed=");   // run by the UI task: no flush meanwhile
      const char *ph0 = strstr(line, "hmin="), *ph1 = strstr(line, "hmax=");
      board_ppa_test_args(pn ? atoi(pn + 2) : 300, ps ? (uint32_t)strtoul(ps + 5, NULL, 10) : 1, ph0 ? atoi(ph0 + 5) : 0,
                          ph1 ? atoi(ph1 + 5) : 0);
      g_rt_done = false;
      g_rt_job = board_ppa_test;
      const uint32_t t0 = clock_ms();
      while (!g_rt_done && clock_ms() - t0 < 60000) wait_ms(20);
      c.printf("HTTP/1.0 200 OK\r\nContent-Type: text/plain\r\nConnection: close\r\n\r\n%s", g_rt_done ? g_rt_out : "timeout\n");
    } else if (route_match(line) || strncmp(line, "GET /i2c ", 9) == 0) {   // run by the UI task: the UI's route
      g_rt_done = false;                             // (bd_debug_route) or the I2C scan (touch reads share the bus)
      g_rt_job = route_match(line) ? g_rt_fn : board_i2c_scan;
      const uint32_t t0 = clock_ms();
      while (!g_rt_done && clock_ms() - t0 < 30000) wait_ms(20);
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
      c.print("HTTP/1.0 404 Not Found\r\nContent-Type: text/plain\r\nConnection: close\r\n\r\nGET /screen.bmp, /rec?every=ms[&max=n], /rec, /rec/stop, /rec.bin, /rec/free, /heap, /tasks, /flash, /roam, /i2c, /ppatest?n=&seed=&hmin=&hmax=, /log[?from=], /log/prev, /loghost?to=, /release[?base=|?check=1|?install=1|?install=<ui>], POST /ota, /touch?x=&y=&ms=, /bl?pct=&hz=\n");
    }
    c.close();
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
  const bd_debug_fn job = g_rt_job;
  if (!job) return;
  g_rt_job = NULL;
  const size_t n = job(g_rt_out, sizeof g_rt_out);
  g_rt_out[n < sizeof g_rt_out ? n : sizeof g_rt_out - 1] = 0;
  g_rt_done = true;
}
#endif  // BREWDECK_DEBUG_HTTP
