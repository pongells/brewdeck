// Log lines to a log server + the UI-loop watch (see logsend.h). Debug builds only.
#include <Arduino.h>
#ifdef BREWDECK_DEBUG_HTTP
#include <WiFi.h>
#include <WiFiUdp.h>
#include <Preferences.h>
#include <string.h>
#include "logsend.h"
#include "board_core.h"
#include "net.h"
#include "tlog.h"
#include "wifi_config.h"

#ifndef LOG_HOST
#define LOG_HOST ""
#endif
#ifndef LOG_PORT
#define LOG_PORT 5140
#endif

static const uint32_t STALL_MS = 5000;
static volatile uint32_t g_loop_ms = 0;          // last loop_mark
static const char *volatile g_loop_at = "setup";

static portMUX_TYPE g_tmux = portMUX_INITIALIZER_UNLOCKED;
static char g_host[64] = "";
static uint16_t g_port = LOG_PORT;
static volatile bool g_target_changed = true;
static char g_target_txt[72] = "";
static volatile uint32_t g_sent = 0, g_send_err = 0, g_resolve_err = 0;   // GET /loghost shows them

uint32_t loop_last_ms(void) { return g_loop_ms; }
void loop_mark(const char *where) {
  g_loop_at = where;
  g_loop_ms = millis();
}

static void parse_target(const char *to) {
  char h[64];
  snprintf(h, sizeof h, "%s", to ? to : "");
  uint16_t port = LOG_PORT;
  char *c = strrchr(h, ':');
  if (c) {
    port = (uint16_t)atoi(c + 1);
    *c = 0;
  }
  portENTER_CRITICAL(&g_tmux);
  snprintf(g_host, sizeof g_host, "%s", h);
  g_port = port ? port : LOG_PORT;
  if (g_host[0]) snprintf(g_target_txt, sizeof g_target_txt, "%s:%u", g_host, (unsigned)g_port);
  else g_target_txt[0] = 0;
  g_target_changed = true;
  portEXIT_CRITICAL(&g_tmux);
}
const char *logsend_target(void) { return g_target_txt; }
void logsend_counts(uint32_t *sent, uint32_t *errors, uint32_t *unresolved) {
  *sent = g_sent;
  *errors = g_send_err;
  *unresolved = g_resolve_err;
}
void logsend_set_target(const char *to) {
  Preferences p;
  if (p.begin("debug", false)) {
    p.putString("loghost", to ? to : "");
    p.putBool("loghost_set", true);
    p.end();
  }
  parse_target(to);
  Log.printf("log: server %s\n", g_target_txt[0] ? g_target_txt : "off");
}

// UI loop watch: one line when it stops, one when it runs again
static void watch_loop(void) {
  static bool stalled = false;
  static uint32_t since = 0;
  const uint32_t last = g_loop_ms, now = millis();
  if (!last) return;                             // before the first pass
  if (!stalled && now - last >= STALL_MS) {
    stalled = true;
    since = last;
    char fl[96];
    board_flush_info(fl, sizeof fl);
    Log.printf("core: UI loop stalled for %u s in \"%s\"; %s\n", (unsigned)((now - last) / 1000), g_loop_at, fl);
  } else if (stalled && g_loop_ms != since) {
    stalled = false;
    Log.printf("core: UI loop running again after %u s\n", (unsigned)((g_loop_ms - since) / 1000));
  }
}

static void task(void *) {
  WiFiUDP udp;
  IPAddress ip;
  bool have_ip = false;
  uint32_t resolved_at = 0;
  uint32_t pos = 0;                              // from the boot line on: sent once Wi-Fi is up (the restart reason)
  char host[64];
  uint16_t port = LOG_PORT;
  static char line[320];
  size_t ln = 0;
  for (;;) {
    vTaskDelay(pdMS_TO_TICKS(250));
    watch_loop();
    if (g_target_changed) {
      portENTER_CRITICAL(&g_tmux);
      memcpy(host, g_host, sizeof host);
      port = g_port;
      g_target_changed = false;
      portEXIT_CRITICAL(&g_tmux);
      have_ip = false;
      resolved_at = 0;
    }
    if (!host[0]) {
      pos = log_written();
      continue;
    }
    if (!net_wifi_up(NULL)) continue;            // keep pos: sent when Wi-Fi is back
    if (!have_ip && (!resolved_at || millis() - resolved_at > 60000)) {
      resolved_at = millis();
      have_ip = ip.fromString(host) || WiFi.hostByName(host, ip) == 1;
      if (!have_ip) { g_resolve_err++; continue; }
    }
    if (!have_ip) continue;
    // whole lines from the memory log, one datagram each (at most 200 per round). A line that cannot go out (lwIP
    // buffers full, e.g. during a screenshot) is tried again next round, up to 3 times, then dropped.
    static int tries = 0;
    char buf[512];
    int sent = 0;
    bool again = false;
    while (sent < 200 && !again) {
      uint32_t next;
      const size_t n = log_read(pos, buf, sizeof buf, &next);
      if (!n) break;
      const uint32_t base = next - n;            // later than pos if the oldest lines were overwritten
      if (base > pos) ln = 0;                    //   then the partial line is gone
      uint32_t done = base;                      // everything before this is sent or dropped
      for (size_t i = 0; i < n; i++) {
        const char ch = buf[i];
        if (ch != '\n') {
          if (ch != '\r' && ln < sizeof line - 1) line[ln++] = ch;   // too long: the rest of it is dropped
          continue;
        }
        if (ln) {
          udp.beginPacket(ip, port);
          udp.printf("<14>1 - %s brewdeck - - - ", board_hostname());   // RFC 5424, user.info, no timestamp
          udp.write((const uint8_t *)line, ln);
          if (!udp.endPacket()) {
            g_send_err++;
            if (++tries < 3) { again = true; break; }   // from this line's start, next round
          } else g_sent++;
          tries = 0;
          sent++;
        }
        ln = 0;
        done = base + i + 1;
      }
      if (again) { pos = done; ln = 0; }         // the failed line is read again from the memory log
      else pos = next;
    }
  }
}

void logsend_begin(void) {
  Preferences p;
  String saved;
  bool set = false;
  if (p.begin("debug", true)) {
    set = p.getBool("loghost_set", false);
    saved = p.getString("loghost", "");
    p.end();
  }
  parse_target(set ? saved.c_str() : LOG_HOST);
  if (g_target_txt[0]) Log.printf("log: sending to %s (UDP)\n", g_target_txt);
  xTaskCreatePinnedToCore(task, "logsend", 4096, NULL, 1, NULL, 0);
}
#endif  // BREWDECK_DEBUG_HTTP
