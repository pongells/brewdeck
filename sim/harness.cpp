// Desktop render test: draws the UI into a RAM framebuffer and writes RGB565 dumps.
#include <lvgl.h>
#include <stdio.h>
#include <string.h>
#include "ui.h"
#include "parse.h"
#include "pb.h"
#include "live.h"

#define W 800
#define H 480
static uint16_t fb[W * H];                         // RGB565, as the panel (sim/topng.py reads it)
static uint16_t buf1[W * 60];

static void flush_cb(lv_display_t *d, const lv_area_t *a, uint8_t *px) {
  uint16_t *c = (uint16_t *)px;
  ui_flush_filter(a->x1, a->y1, a->x2, a->y2, c, a->x2 - a->x1 + 1);    // the pressure-release glow, as on the device
  for (int y = a->y1; y <= a->y2; y++)
    for (int x = a->x1; x <= a->x2; x++) fb[y * W + x] = *c++;
  lv_display_flush_ready(d);
}

static void pump(void) {
  for (int i = 0; i < 6; i++) { lv_tick_inc(20); lv_timer_handler(); }
}

static void dump(const char *name) {
  pump();
  FILE *f = fopen(name, "wb");
  fwrite(fb, 2, W * H, f);
  fclose(f);
  printf("wrote %s\n", name);
}

int main(int argc, char **argv) {
  const char *dir = argc > 1 ? argv[1] : ".";
  char path[256];
  lv_init();
  lv_display_t *disp = lv_display_create(W, H);
  lv_display_set_color_format(disp, LV_COLOR_FORMAT_RGB565);
  lv_display_set_buffers(disp, buf1, NULL, sizeof buf1, LV_DISPLAY_RENDER_MODE_PARTIAL);
  lv_display_set_flush_cb(disp, flush_cb);
  ui_init();

  snprintf(path, sizeof path, "%s/ready.raw", dir); dump(path);

  ui_test_open_editor(); ui_test_cursor(1);
  snprintf(path, sizeof path, "%s/editor_empty.raw", dir); dump(path);

  ui_test_key('1'); ui_test_key('9'); ui_test_cursor(1);
  snprintf(path, sizeof path, "%s/editor_19.raw", dir); dump(path);

  ui_test_key('b'); ui_test_key('b'); ui_test_key('1'); ui_test_key('8'); ui_test_key('.'); ui_test_key('5'); ui_test_key('9'); ui_test_cursor(1);
  snprintf(path, sizeof path, "%s/editor_18_5.raw", dir); dump(path);

  ui_test_cancel();
  ui_test_open_temp_editor(); ui_test_key('9'); ui_test_key('.'); ui_test_key('3'); ui_test_cursor(1);   // '.' ignored
  snprintf(path, sizeof path, "%s/editor_temp_93.raw", dir); dump(path);
  ui_test_cancel();
  {
    bd_machine d; memset(&d, 0, sizeof d); d.readiness = BD_RD_UNKNOWN; d.commands_ready = true;
    d.temperature = 90.509f; d.target_temp = 90; d.water_level = 30;
    strcpy(d.profile_name, "Leva 9 SINGLE v0.9 (community)"); strcpy(d.mode, "HOME");
    d.link = BD_LINK_ONLINE; ui_set_live(&d);
    snprintf(path, sizeof path, "%s/live_ready.raw", dir); dump(path);
    d.scale_present = true; d.weight = 101.9f; d.scale_type = BD_SCALE_BT;   // scale pill per type
    d.link = BD_LINK_ONLINE; ui_set_live(&d);
    snprintf(path, sizeof path, "%s/live_ready_scale.raw", dir); dump(path);
    d.scale_type = BD_SCALE_WIRED;
    d.link = BD_LINK_ONLINE; ui_set_live(&d);
    snprintf(path, sizeof path, "%s/live_scale_hw.raw", dir); dump(path);
    d.scale_type = BD_SCALE_PREDICTIVE; d.scale_present = false;
    d.link = BD_LINK_ONLINE; ui_set_live(&d);
    snprintf(path, sizeof path, "%s/live_scale_pred.raw", dir); dump(path);
    d.scale_type = BD_SCALE_BT;
    d.link = BD_LINK_ONLINE; ui_set_live(&d);
    snprintf(path, sizeof path, "%s/live_scale_bt_off.raw", dir); dump(path);
    d.scale_present = false; d.weight = 0; d.scale_type = BD_SCALE_UNKNOWN;
    d.temperature = 72.3f; d.warmup_remaining = 80; d.water_level = 12;
    d.link = BD_LINK_ONLINE; ui_set_live(&d);
    snprintf(path, sizeof path, "%s/live_heating.raw", dir); dump(path);
    d.temperature = 91.2f; d.warmup_remaining = 0; d.brewing = true; d.scale_present = true; d.weight = 18.4f; d.water_level = 78;
    d.link = BD_LINK_ONLINE; ui_set_live(&d);
    snprintf(path, sizeof path, "%s/live_brewing.raw", dir); dump(path);
    ui_set_addr("192.168.1.42", "192.168.1.50");
    d.link = BD_LINK_NO_MACHINE; ui_set_live(&d);
    snprintf(path, sizeof path, "%s/live_offline_0s.raw", dir); dump(path);   // < 5 s: Ready screen still shown
    lv_tick_inc(5200); lv_timer_handler();
    snprintf(path, sizeof path, "%s/live_offline.raw", dir); dump(path);      // offline screen
    d.link = BD_LINK_CONNECTING; ui_set_live(&d);
    lv_tick_inc(5200); lv_timer_handler();
    snprintf(path, sizeof path, "%s/live_nowifi.raw", dir); dump(path);
    d.link = BD_LINK_ONLINE; ui_set_live(&d);
    lv_tick_inc(300); lv_timer_handler();
    snprintf(path, sizeof path, "%s/live_back.raw", dir); dump(path);         // back: offline screen gone
    ui_set_clock(7, 52);
    d.brewing = false; d.temperature = 90.1f;
    d.link = BD_LINK_ONLINE; ui_set_live(&d);
    lv_tick_inc(10 * 60000 + 600); lv_timer_handler();
    snprintf(path, sizeof path, "%s/idle.raw", dir); dump(path);              // idle screen after 10 min
    ui_test_wake(); lv_tick_inc(600); lv_timer_handler();                   // a touch wakes it (the idle clock
                                                                            // only goes on a touch; trig_activity
                                                                            // left it over the later renders)
    d.scale_present = true; d.scale_type = BD_SCALE_BT; d.weight = 0.4f;
    d.link = BD_LINK_ONLINE; ui_set_live(&d);
    ui_set_wifi("HomeMesh", -61, 6, "02:00:00:00:00:02");
    ui_set_sysinfo("Nov  5 2026 00:40", "", "0 h 12 min", "84 KB internal, 6.1 MB PSRAM");
    static const char *const MP[] = {"menu", "menu_display", "menu_scale", "menu_wifi", "menu_about"};
    for (int k = -1; k < 4; k++) {
      ui_test_menu(k); lv_tick_inc(1100); lv_timer_handler();
      snprintf(path, sizeof path, "%s/%s.raw", dir, MP[k + 1]); dump(path);
    }
    ui_test_menu(-2);
  }
  {
    // profile picker with a real machine's list; details missing for one row to show "Loading..."
    static const struct { int id; const char *name; int ph; float w, t; } P[] = {
      {1, "IUIUIU Classic", 4, 36, 93}, {3, "Londinium", 0, 0, 0}, {4, "Adaptive", 5, 40, 93},
      {5, "Filter 2.1", 6, 0, 93}, {6, "Blooming espresso", 4, 40, 92}, {7, "LMD Single v1.1 (community)", 6, 38, 91},
      {8, "Leva 9 SINGLE v0.9 (community)", 19, 21, 90}, {9, "[UT] Tube Fill (community)", 1, 0, 90},
      {10, "[UT] Test OPV (community)", 2, 0, 90}};
    static bd_profiles L; memset(&L, 0, sizeof L);
    L.count = 9;
    for (int i = 0; i < 9; i++) {
      bd_profile *p = &L.p[i];
      p->id = P[i].id; strcpy(p->name, P[i].name); p->active = P[i].id == 8;
      p->detail = P[i].id != 3; p->phases = P[i].ph; p->stop_weight = P[i].w; p->brew_temp = P[i].t;
    }
    bd_machine d; memset(&d, 0, sizeof d); d.readiness = BD_RD_UNKNOWN; d.commands_ready = true;
    d.temperature = 90.1f; d.target_temp = 90; d.water_level = 18;
    strcpy(d.profile_name, "Leva 9 SINGLE v0.9 (community)"); strcpy(d.mode, "HOME");
    d.link = BD_LINK_ONLINE; ui_set_live(&d);
    ui_set_profiles(&L);
    ui_test_profiles(1, 0);
    snprintf(path, sizeof path, "%s/profiles.raw", dir); dump(path);
    ui_test_profiles(1, 1);
    snprintf(path, sizeof path, "%s/profiles_all.raw", dir); dump(path);
    ui_test_profiles(0, 0);
    bd_profiles E; memset(&E, 0, sizeof E);
    ui_set_profiles(&E);
    ui_test_profiles(1, 0);
    snprintf(path, sizeof path, "%s/profiles_empty.raw", dir); dump(path);
    ui_test_profiles(0, 0);
  }
  {
    // profile preview + target weight from real profiles (sim/data, GET /api/profile/<id>)
    static const int ids[] = {8, 1, 4, 5, 7};
    bd_machine d; memset(&d, 0, sizeof d); d.readiness = BD_RD_UNKNOWN; d.commands_ready = true;
    d.temperature = 90.1f; d.target_temp = 90; d.water_level = 18; strcpy(d.mode, "HOME");
    for (int id : ids) {
      char jp[256]; snprintf(jp, sizeof jp, "%s/data/profile_%d.json", SIM_DIR, id);
      FILE *f = fopen(jp, "rb"); if (!f) { printf("missing %s\n", jp); return 1; }
      static char js[16384]; size_t n = fread(js, 1, sizeof js - 1, f); js[n] = 0; fclose(f);
      static bd_profile_shape sh;
      if (!parse_profile_shape(js, &sh)) { printf("parse failed %s\n", jp); return 1; }
      sh.id = id;
      char nm[48] = ""; const char *q = strstr(js, "\"name\":\""); if (q) sscanf(q + 8, "%47[^\"]", nm);
      strcpy(d.profile_name, nm);
      d.link = BD_LINK_ONLINE; ui_set_live(&d);
      ui_set_shape(&sh);
      snprintf(path, sizeof path, "%s/preview_%d.raw", dir, id); dump(path);
    }
  }
  {
    // brewing screen: play back the demo shot (sim/data/ws_demo_shot.bin, LMD Single, stored as 341) through the real decoder
    static bd_profile_shape sh;
    { char jp[256]; snprintf(jp, sizeof jp, "%s/data/profile_7.json", SIM_DIR); FILE *f = fopen(jp, "rb");
      static char js[16384]; size_t n = fread(js, 1, sizeof js - 1, f); js[n] = 0; fclose(f);
      parse_profile_shape(js, &sh); sh.id = 7; ui_set_shape(&sh); }
    char bp[256]; snprintf(bp, sizeof bp, "%s/data/ws_demo_shot.bin", SIM_DIR);
    FILE *f = fopen(bp, "rb"); if (!f) { printf("missing %s\n", bp); return 1; }
    static uint8_t b[1 << 20]; size_t n = fread(b, 1, sizeof b, f); fclose(f);
    bd_machine d; memset(&d, 0, sizeof d); d.readiness = BD_RD_UNKNOWN; d.commands_ready = true;
    d.target_temp = 91; d.water_level = 20; strcpy(d.profile_name, "LMD Single v1.1 (community)"); strcpy(d.mode, "HOME");
    static const int at[] = {50, 120, 200}; int next = 0; uint32_t last_t = 0;
    // the core's shot start / end rule (core_state.cpp): end 0.3 s after brewing off with no new point, or after 3 s
    uint32_t vt = 0, last_pt = 0; bool started = false, ended = false, brewed = false;
    auto tick = [&](uint32_t ms) { lv_tick_inc(ms); vt += ms; };
    auto end_check = [&]() {
      if (started && !ended && ((brewed && !d.brewing && vt - last_pt > 300) || vt - last_pt > 3000)) {
        ended = true; ui_shot_end();
      }
    };
    for (size_t i = 0; i + 4 <= n;) {
      uint32_t L = b[i] | b[i+1] << 8 | b[i+2] << 16 | (uint32_t)b[i+3] << 24; const uint8_t *fr = b + i + 4; i += 4 + L;
      char act[24]; const uint8_t *dd; size_t dl;
      if (!pb_envelope(fr, L, act, sizeof act, &dd, &dl)) continue;
      if (!strcmp(act, "d_sensor_snap")) {
        WsSensor s; pb_sensor(dd, dl, &s);
        d.temperature = s.temperature; d.pressure = s.pressure; d.brewing = s.brew_active; d.water_level = s.water_level;
        if (d.brewing && started) brewed = true;
        d.link = BD_LINK_ONLINE; ui_set_live(&d); tick(100); end_check(); lv_timer_handler();
      } else if (!strcmp(act, "d_shot_snap")) {
        WsShot s; pb_shot(dd, dl, &s);
        if (s.time10 > last_t) { tick((s.time10 - last_t) * 100 > 600 ? 600 : (s.time10 - last_t) * 100); last_t = s.time10; }
        if (!started) { started = true; brewed = d.brewing; ui_shot_start(); }
        bd_shot_point pt; pb_shot_point(&s, &pt);
        ui_shot(&pt); last_pt = vt; lv_timer_handler();
        if (next < 3 && (int)s.time10 >= at[next]) { snprintf(path, sizeof path, "%s/brew_%02d.raw", dir, at[next] / 10); dump(path); next++; }
      }
    }
    for (int k = 0; k < 10; k++) { tick(100); end_check(); lv_timer_handler(); }
    snprintf(path, sizeof path, "%s/brew_done.raw", dir); dump(path);
    for (int k = 0; k < 30; k++) { lv_tick_inc(100); lv_timer_handler(); }    // 3 s later: shot review
    snprintf(path, sizeof path, "%s/review.raw", dir); dump(path);
    ui_test_review(0, 120); lv_timer_handler();                                 // scrubbed to 12 s
    snprintf(path, sizeof path, "%s/review_scrub.raw", dir); dump(path);
    {   // previous shot: the stored shot before it (sim/data/shot_340.json, GET /api/shots/340)
      static bd_shot prev; static char js[65536]; char jp[256];
      snprintf(jp, sizeof jp, "%s/data/shot_340.json", SIM_DIR); FILE *g = fopen(jp, "rb");
      size_t m = fread(js, 1, sizeof js - 1, g); js[m] = 0; fclose(g);
      if (!parse_shot(js, &prev)) { printf("shot_340.json did not parse\n"); return 1; }
      ui_set_prev(2 /* PREV_READY */, &prev);
      ui_test_review(1, 120); lv_timer_handler();
      snprintf(path, sizeof path, "%s/review_prev.raw", dir); dump(path);
    }
    for (int k = 0; k < 650; k++) { lv_tick_inc(100); lv_timer_handler(); }   // 65 s untouched: back to Ready
    snprintf(path, sizeof path, "%s/brew_closed.raw", dir); dump(path);
  }
  ui_test_done();
  ui_test_step(5); ui_test_step(5);
  snprintf(path, sizeof path, "%s/ready_after.raw", dir); dump(path);
  return 0;
}
