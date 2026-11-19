// Brewdeck's bd_ui_*: the core's state and events into the screens (ui_set_*), the UI's settings and originals.
#include <stddef.h>
#include <stdio.h>
#include <string.h>
#include "../core/brewdeck_core.h"
#include "../core/bd_lvgl9.h"
#include "ui.h"

// Shared by the device, the emulator and the web demo (each with its own core). The screens send their commands with
// bd_* themselves.

extern "C" const bd_ui_desc BD_UI = {"screen", "1", 1};          // "screen": the NVS namespace of its settings

// each profile's original values (screens/profiles.cpp): key "p" + FNV-1a of the name (storage keys are at most 15
// chars), value = weight x10 << 16 | degC
static void orig_key(const char *n, char *k, size_t kn) {
  uint32_t h = 2166136261u;
  for (; *n; n++) { h ^= (uint8_t)*n; h *= 16777619u; }
  snprintf(k, kn, "p%08x", (unsigned)h);
}
static bool orig_load(const char *p, int *w10, int *deg) {
  char k[12];
  orig_key(p, k, sizeof k);
  const uint32_t v = (uint32_t)bd_store_get_int(k, 0);
  if (!v) return false;
  *w10 = (int)(v >> 16);
  *deg = (int)(v & 0xFFFF);
  return true;
}
static void orig_save(const char *p, int w10, int deg) {
  char k[12];
  orig_key(p, k, sizeof k);
  bd_store_set_int(k, (int32_t)(((uint32_t)w10 << 16) | (uint32_t)(deg & 0xFFFF)));
  bd_log("originals: \"%s\" %d.%d g, %d C kept", p, w10 / 10, w10 % 10, deg);
}

// screen settings from the menu, written 2 s after the last change
static int g_bright = 100, g_idle = 10;
static bool g_bounce = false, g_splash = true;
static uint32_t g_set_dirty = 0, g_now = 0;
static void on_settings(int bright, int idle_min, bool bounce, bool splash) {
  g_bright = bright;
  g_idle = idle_min;
  g_bounce = bounce;
  g_splash = splash;
  g_set_dirty = g_now ? g_now : 1;
}

// ---- state: bd_* -> the screens -------------------------------------------------------------------------------------
static const char *const RESET_TXT[] = {"since power-on", "after the reset pin", "after a software restart",
                                        "after a crash", "after a freeze (watchdog)", "after a power dip",
                                        "after deep sleep", "after a USB reset", "after an unknown reset"};
static void info_tick(void) {
  const bd_network *n = bd_network_state();
  const bd_machine *m = bd_machine_state();
  ui_set_addr(n->screen_ip, n->machine_where);
  ui_set_wifi_target(n->target);
  ui_set_offline_reason(m->why, n->why_ip);
  ui_set_machine_host(n->machine_addr);
  ui_set_wifi(n->ssid, n->rssi, n->channel, n->ap);
  const bd_core_info *c = bd_core();
  char up[64], mem[48];
  const uint32_t mn = c->uptime_s / 60;
  const int rs = c->reset >= 0 && c->reset <= BD_RESET_OTHER ? (int)c->reset : (int)BD_RESET_OTHER;
  snprintf(up, sizeof up, "%u h %u min \xC2\xB7 %s", (unsigned)(mn / 60), (unsigned)(mn % 60), RESET_TXT[rs]);
  // the emulator does not measure memory
  if (!c->free_internal && !c->free_psram) snprintf(mem, sizeof mem, "n/a (desktop)");
  else snprintf(mem, sizeof mem, "%u KB internal, %.1f MB PSRAM", (unsigned)(c->free_internal / 1024),
                c->free_psram / 1048576.0);
  ui_set_sysinfo(c->build, up, mem);
  int h, mi;
  if (bd_local_time(&h, &mi, NULL)) ui_set_clock(h, mi);
  else ui_set_clock(-1, -1);
}

// ---- bd_ui_* --------------------------------------------------------------------------------------------------------
#ifdef BREWDECK_DEBUG_HTTP
static size_t bench_run(char *out, size_t cap);   // GET /bench, below
#endif
void bd_ui_init(void) {
  bd_lvgl9_setup();
  bd_lvgl9_flush_filter = ui_flush_filter;   // the pressure-release glow lies over everything
  ui_init();
  ui_on_originals(orig_load, orig_save);
  g_bright = bd_store_get_int("bright", 100);
  g_idle = bd_store_get_int("idle", 10);
  g_bounce = bd_store_get_int("bounce", 0) != 0;
  g_splash = bd_store_get_int("splash", 1) != 0;
  // brightness below 20 % is refused by ui_set_settings
  ui_set_settings(g_bright, g_idle, g_bounce, g_splash);
  if (g_splash) ui_splash();       // Display > "Splash at start" (on by default), after the saved settings
  ui_on_settings(on_settings);
#ifdef BREWDECK_DEBUG_HTTP
  bd_debug_route("/bench", bench_run);   // debug builds: redraw timings per screen
#endif
}

#ifdef BREWDECK_DEBUG_HTTP
#include <esp_timer.h>
// GET /bench (debug): per screen the opening (page shown + first redraw) and a full redraw 3 times, with the part spent
// copying to the panel (tools: curl -4 http://brewdeck.local/bench)
static size_t bench_run(char *out, size_t cap) {
  char *o = out;
  size_t left = cap;
  int n = snprintf(o, left, "%-14s %8s %8s %8s %8s\n", "screen", "open ms", "best ms", "avg ms", "flush ms");
  o += n; left -= n;
  for (int i = 0;; i++) {
    lv_refr_now(NULL);
    const int64_t o0 = esp_timer_get_time();       // opening: building / showing the page and its first redraw
    const char *name = ui_bench_show(i);
    if (!name) break;
    lv_refr_now(NULL);
    const uint32_t open_us = (uint32_t)(esp_timer_get_time() - o0);
    uint32_t best = ~0u, sum = 0, fl = 0;
    for (int k = 0; k < 3; k++) {
      lv_obj_invalidate(lv_screen_active());
      lv_obj_invalidate(lv_layer_top());
      bd_display_stats(NULL, NULL, true);
      const int64_t t0 = esp_timer_get_time();
      lv_refr_now(NULL);
      const uint32_t us = (uint32_t)(esp_timer_get_time() - t0);
      uint32_t fus;
      bd_display_stats(&fus, NULL, false);
      if (us < best) best = us;
      sum += us;
      fl += fus;
    }
    n = snprintf(o, left, "%-14s %8.1f %8.1f %8.1f %8.1f\n", name, open_us / 1000.0, best / 1000.0, sum / 3000.0,
                 fl / 3000.0);
    if (n > 0 && (size_t)n < left) { o += n; left -= n; }
  }
  lv_refr_now(NULL);
  return cap - left;
}
#endif

void bd_ui_tick(uint32_t now) {
  g_now = now;
  static uint32_t seen_m, seen_pl, seen_sh, seen_hi, seen_net, last_info;
  if (bd_seq_machine() != seen_m) {
    seen_m = bd_seq_machine();
    ui_set_live(bd_machine_state());
  }
  if (bd_seq_network() != seen_net && now - last_info >= 1000) {   // once a second, like the core's network info
    seen_net = bd_seq_network();
    last_info = now;
    info_tick();
  }
  if (bd_seq_profiles() != seen_pl) {
    seen_pl = bd_seq_profiles();
    ui_set_profiles(bd_profile_list());
  }
  if (bd_seq_shape() != seen_sh) {
    seen_sh = bd_seq_shape();
    ui_set_shape(bd_active_shape());
  }
  if (bd_seq_history() != seen_hi && bd_shot_history()) {
    seen_hi = bd_seq_history();
    ui_set_shot_index(bd_shot_history());
  }
  if (g_set_dirty && now - g_set_dirty > 2000) {
    g_set_dirty = 0;
    bd_store_set_int("bright", g_bright);
    bd_store_set_int("idle", g_idle);
    bd_store_set_int("bounce", g_bounce);
    bd_store_set_int("splash", g_splash);
    bd_log("settings saved: brightness %d %%, idle %d min, idle clock %s", g_bright, g_idle,
           g_bounce ? "bouncing" : "still");
  }
  bd_lvgl9_tick(now);
}

void bd_ui_event(const bd_event *e) {
  switch (e->type) {
    case BD_EV_SHOT_START: ui_shot_start(); break;
    case BD_EV_SHOT_POINT: ui_shot(e->u.point); break;
    case BD_EV_SHOT_END: ui_shot_end(); break;
    case BD_EV_NOTICE:
      if (e->u.notice.update) ui_update_notice(e->u.notice.type, bd_machine_state()->update);
      else ui_notify(e->u.notice.type, e->u.notice.text);
      break;
    case BD_EV_PRESSURE_RELEASE: ui_pressure_release(e->u.release.count); break;
    case BD_EV_COMMAND: ui_command_answer(e->u.command.cmd, e->u.command.answer); break;
    case BD_EV_SHOT_LOADED:
      if (e->u.loaded.history)
        ui_set_hist(e->u.loaded.state, e->u.loaded.shot, e->u.loaded.shape,
                    e->u.loaded.previous, e->u.loaded.previous_state);
      else ui_set_prev(e->u.loaded.state, e->u.loaded.shot);
      break;
    case BD_EV_WIFI_SCAN: ui_set_wifi_scan(e->u.scan.nets, e->u.scan.n, e->u.scan.scanning); break;
    case BD_EV_WIFI_JOIN: ui_set_wifi_join(e->u.join.state); break;
    default: break;
  }
}

void bd_ui_wake(void) {}           // ui.cpp wakes itself on shots, steam, flush and notices (ui_set_live, ui_notify)
