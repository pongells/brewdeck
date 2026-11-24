// Internal to Brewdeck's UI (ui.cpp + screens/*.cpp): what the screen files share. The public side is ui.h.
#pragma once
#include <ctype.h>
#include <stdint.h>
#include <lvgl.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>
#include <stdlib.h>
#include <math.h>
#include <time.h>
#ifndef LV_SIM_HOST
#include <esp_heap_caps.h>
#endif
#include "../core/brewdeck_core.h"   // commands, shot loading, Wi-Fi and backlight go to the core
#include "ui.h"
#include "../core/preview.h"
#include "theme.h"

// A "since" timestamp where 0 means "not set". Never write `now | 1`: on an even tick that is 1 ms in the future, so an
// elapsed check in the same millisecond wraps to ~49 days and passes at once. 0 maps to UINT32_MAX = 1 ms in the past.
static inline uint32_t stamp_nz(uint32_t now) { return now ? now : 0xFFFFFFFFu; }
// a send-later timer (user data 1 while a change waits to be sent)
static inline bool timer_pending(lv_timer_t *t) { return t && lv_timer_get_user_data(t) == (void *)1; }
// FNV-1a over n bytes, continuing from h (start with FNV_SEED): fingerprints of what a page shows
static const uint32_t FNV_SEED = 2166136261u;
static inline uint32_t fnv1a(uint32_t h, const void *p, size_t n) {
  for (size_t i = 0; i < n; i++) { h ^= ((const uint8_t *)p)[i]; h *= 16777619u; }
  return h;
}
// a Unix time before this (Sep 2020) means the clock was not set (no NTP yet, or the machine's clock)
static const uint32_t CLOCK_SET_TS = 1600000000u;

// ---- shared constants and types -------------------------------------------------------------------------------------
static_assert(sizeof(lv_point_precise_t) == sizeof(lv_point_t), "point layout (LV_USE_FLOAT 0)");
// stop weight in tenths of a gram: the range the screen edits, and the -/+ step
static const int W_MIN10 = 50, W_MAX10 = 800, W_STEP10 = 5;
// a stop weight from the machine into the range the screen edits (0 = no stop weight stays 0)
static inline int clamp_w10(int w10) { return w10 <= 0 ? 0 : w10 < W_MIN10 ? W_MIN10 : w10 > W_MAX10 ? W_MAX10 : w10; }
// brew temperature: 1-105 degC (0 = not known / heater off, never sent); a + from an unknown value starts at T_START
static const int T_MIN = 1, T_MAX = 105, T_START = 60;
static const int VALS_Y = 404;                      // brewing / steam: the values row under the chart (y 404..456)
enum { IC_HISTORY, IC_SCALE, IC_DISPLAY, IC_WIFI, IC_ABOUT, IC_PROFILES, IC_SETTINGS, IC_BELL };   // line_icon kinds
struct EmptyState { lv_obj_t *box, *title, *body; };
struct Restore { int id; char name[48]; int w10, deg; uint32_t t; };   // a restore waiting for its profile
enum { RV_PREV_NONE = 0, RV_PREV_LOADING, RV_PREV_READY, RV_PREV_MISSING, RV_PREV_FAILED };   // = bd_load (BD_LOAD_*)
// menu pages (g_menu_page, menu_show_page); the address page uses the password page's objects
enum { MP_LIST = -1, MP_DISPLAY, MP_SCALE, MP_WIFI, MP_ABOUT, MP_WIFI_DETAILS, MP_PASSWORD, MP_ADDRESS, MP_HISTORY,
       MENU_PAGES };
// brewing screen / shot review (g_br_state)
enum { BR_HIDDEN = 0, BR_BREWING, BR_DONE, BR_REVIEW };
// the flush button: the machine's mode as bd_flush (BD_FLUSH_OFF = HOME), FL_UNKNOWN = busy, offline or not asked
enum { FL_UNKNOWN = -1 };

// ---- screens/common.cpp: drawing and object helpers, icons, chart helpers -------------------------------------------
void dl(lv_layer_t *L, lv_draw_line_dsc_t *d, const lv_point_t *a, const lv_point_t *b);
void darc(lv_layer_t *L, lv_draw_arc_dsc_t *d, const lv_point_t *c, int r, int a0, int a1);
void dlabel(lv_layer_t *L, lv_draw_label_dsc_t *d, const lv_area_t *a, const char *t);
lv_obj_t *box(lv_obj_t *p, int x, int y, int w, int h, lv_color_t bg, int r);
void one_line(lv_obj_t *l, int32_t w);
void hairline(lv_obj_t *o, lv_border_side_t side);
lv_obj_t *label(lv_obj_t *p, const char *text, const lv_font_t *font, lv_color_t col);
int base_pad(const lv_font_t *big, const lv_font_t *small);
void digit_dot(lv_obj_t *row, const lv_font_t *f, lv_color_t c, int d);
lv_obj_t *logo(lv_obj_t *p, int size);
lv_obj_t *line_icon(lv_obj_t *p, int kind, int size, lv_color_t col);
EmptyState empty_state(lv_obj_t *p, int kind);
void empty_set(const EmptyState &s, const char *title, const char *body);
lv_obj_t *flex(lv_obj_t *p, lv_flex_flow_t flow, int gap);
void make_button(lv_obj_t *o, lv_color_t pressed);
lv_obj_t *line(lv_obj_t *p, const lv_point_t *pts, uint16_t n, int width, lv_color_t col);
void set_text(lv_obj_t *l, const char *s);
void set_bg(lv_obj_t *o, lv_color_t c);
void set_tc(lv_obj_t *o, lv_color_t c);
void set_line_c(lv_obj_t *o, lv_color_t c);
void set_hidden(lv_obj_t *o, bool h);
void set_h(lv_obj_t *o, int32_t h);
void set_y(lv_obj_t *o, int32_t y);
void display_name(const char *in, char *out, size_t n, bool *community);
lv_obj_t *icon_box(lv_obj_t *p, int w, int h);
lv_obj_t *icon_tank(lv_obj_t *p, int pct, lv_color_t fill);
void icon_chevron_right(lv_obj_t *p, lv_color_t col);
void icon_check(lv_obj_t *p, lv_color_t col);
void icon_close(lv_obj_t *p, lv_color_t col);
void icon_minus(lv_obj_t *p, lv_color_t col);
void icon_plus(lv_obj_t *p, lv_color_t col);
void icon_backspace(lv_obj_t *p, lv_color_t col);
void ch_hline(lv_layer_t *ctx, int x1, int x2, int y, lv_color_t col);
void ch_text_c(lv_layer_t *ctx, int x, int y, const char *s, lv_text_align_t align, lv_color_t col);
// time labels under a chart of span_s seconds that starts at from_s (a rolling window), every step_s, end label at the
// right end
void axis_time(lv_layer_t *ctx, int x0, int w, int y, int span_s, int step_s, int from_s = 0);
lv_obj_t *strip_value(lv_obj_t *row, lv_color_t dot, const char *unit);
void fmt_when(uint32_t ts, char *b, size_t n);
void fmt_day(uint32_t ts, char *b, size_t n);
int today_key(void);
lv_obj_t *pill_row(lv_obj_t *p);
// tags for the shared UI scenarios (docs/ui-features.md, tests/ui): names on the objects that matter, no effect on
// the look. tag_marker: an invisible 1 x 1 child that shows while what it stands for does (a warning that is only a
// colour on an object that is always there); tag_markers_show switches the markers of an object.
static inline void tag(lv_obj_t *o, const char *t) {   // t: a string that lives on (a literal); NULL removes the tag
  if (t) lv_obj_set_name_static(o, t);
  else lv_obj_set_name(o, NULL);
}
lv_obj_t *tag_marker(lv_obj_t *p, const char *t);
void tag_markers_show(lv_obj_t *p, bool on);

// ---- screens/ready.cpp: Ready, its keypad, the flush button ---------------------------------------------------------
extern int g_weight10;
extern char g_cur_profile[48];
extern lv_obj_t *g_editor;
extern lv_obj_t *g_cursor;
extern bool g_cursor_on;
extern lv_obj_t *g_pill, *g_pill_dot, *g_pill_lbl;
extern lv_obj_t *g_tank, *g_tank_fill, *g_tank_lbl, *g_tank_pct;
extern lv_obj_t *g_ready;
void ready_cover(void);   // hide Ready while brewing, the review or steam covers it
extern lv_obj_t *g_prof_lbl;
extern lv_obj_t *g_temp_lbl;
extern lv_obj_t *g_scale_lbl;
extern lv_obj_t *g_hdr_left;
extern int32_t g_hdr_btn_x;
extern lv_obj_t *g_menu_btn;
extern lv_obj_t *g_pv_msg;
extern bool g_shape_loaded;
extern bd_profile_shape g_last_shape;
extern int g_shape_id;
extern lv_obj_t *g_tt_lbl, *g_tw_lbl;
extern bool g_can_select;
extern int g_tset;
extern int g_act_temp;
extern lv_timer_t *g_temp_tm;
extern lv_timer_t *g_weight_tm;
extern int g_act_weight10;
extern bool g_can_tare;
extern lv_obj_t *g_tare_lbl;
extern lv_obj_t *g_sc_ic[4], *g_scale_name, *g_scale_pill;
extern lv_obj_t *g_pr_pill, *g_pr_ic[2], *g_pr_name, *g_pr_lbl;
extern int g_fl_mode;
extern bool g_fl_idle;
extern int g_fl_want;
void format_weight(char *out, size_t n, int tenths);
void refresh_value(void);
void step_weight(int delta10);
void weight_commit(lv_timer_t *t);   // send a waiting stop-weight change now (NULL from code)
void refresh_tset(void);
void temp_commit(lv_timer_t *t);
void editor_open(bool temp);
void editor_close(void);
void editor_key(char k);
void editor_done(void);
void pv_loading(void);
void show_profile(const char *raw);
void foot_pill(lv_obj_t *p, bool warn, int32_t pl, int32_t pr);
void stepper_part(lv_obj_t *o);
void fl_show(void);
void build_ready(lv_obj_t *scr);
void build_editor(lv_obj_t *scr);

// ---- screens/profiles.cpp: the Profiles page, originals and snapshots -----------------------------------------------
extern lv_obj_t *g_pick;
extern bd_profiles g_profiles;
extern int g_pend_id;
extern char g_pend_name[48];
extern bool g_orig_have;
extern int g_orig_w10, g_orig_deg;
extern Restore g_rst;
int prof_id(const char *name);
void picker_close(void);
void picker_open(void);
void on_pick_open(lv_event_t *e);
void picker_rebuild(void);
void build_picker(lv_obj_t *scr);
void orig_sync(void);
void orig_note(void);
bool orig_temp_edited(void);
bool orig_weight_edited(void);
void orig_paint(void);
void snap_apply(void);

// ---- screens/steam.cpp ----------------------------------------------------------------------------------------------
extern lv_obj_t *g_st;
extern bool g_st_on;   // the steam screen is shown
void st_update(const bd_machine *d, bool on);
void build_steam(lv_obj_t *scr);

// ---- screens/brew.cpp: brewing screen and shot review ---------------------------------------------------------------
extern bd_shot_point *g_br;
extern int g_br_n;
extern int g_br_state;
extern bool g_live_brew;
extern lv_obj_t *g_brew;
extern int g_rv_prev_state;
extern int g_rv_mode;
extern int g_rv_i;
extern lv_obj_t *g_rv;
extern lv_obj_t *g_rv_chart;
void br_hide(void);
void br_steam_started(void);
void rv_values(void);
void rv_axis(void);
void rv_toggle_style(void);
void rv_open_stored(const bd_shot *s, const bd_profile_shape *sh, const bd_shot *prev, int prev_state);
void build_brew(lv_obj_t *scr);

// ---- screens/offline.cpp --------------------------------------------------------------------------------------------
extern lv_obj_t *g_off;
extern int g_off_link;
extern uint32_t g_off_since;
extern uint32_t g_off_phase;
extern bool g_off_ever_online;
extern bool g_off_active;
extern char g_off_sip[20];
extern char g_off_mach[40];
void on_off_timer(lv_timer_t *t);
void build_offline(lv_obj_t *scr);

// ---- screens/idle.cpp: idle clock, screen off, splash ---------------------------------------------------------------
extern int g_idle_min;
extern int g_bright;
extern int g_idle_bright;
extern lv_obj_t *g_idle;
extern int g_clock_h, g_clock_m;
extern bool g_idle_busy;
extern char g_idle_temp[16];
extern int g_idle_state;
extern bool g_press_warn;
extern float g_press_val;
extern int g_idle_word;
extern bool g_idle_force;
extern bool g_idle_bounce;
extern lv_obj_t *g_blank;
extern bool g_blank_on;
extern bool g_blank_offline;
extern bool g_splash_on;
void bl_set(int pct);
void idle_hide(void);
void on_idle_timer(lv_timer_t *t);
void build_idle(lv_obj_t *scr);
void build_blank(lv_obj_t *scr);
void ui_screen_wake(void);      // lit and on the screen below: out of screen off and the dimmed clock

// ---- screens/update.cpp: a firmware update arriving over Wi-Fi (BD_EV_UPDATE) ----------------------------------------
void build_update(void);

// ---- screens/notices.cpp: banner, pressure-release glow, notices list -----------------------------------------------
extern char g_upd_rel[32];
extern bool g_upd_known;
extern bool g_upd_shown;
void ntc_build_btn(lv_obj_t *scr, int32_t x);
void build_notif(void);
void build_frame(void);

// ---- screens/menu.cpp: the menu and its Display, Scale, About pages -------------------------------------------------
extern lv_obj_t *g_menu;
extern lv_obj_t *g_menu_page[MENU_PAGES];
extern int g_menu_cur;
extern int g_menu_entry;
extern bool g_in_wifi_pages;
extern bd_machine g_menu_live;
extern int g_menu_link;
void menu_show_page(int k);
void on_menu_open(lv_event_t *e);
void open_wifi_menu(void);
void menu_close(void);
void build_menu(lv_obj_t *scr);

// ---- screens/wifi.cpp: Wi-Fi list, details, password and address keyboard -------------------------------------------
extern char g_wifi_ssid[33];
extern char g_wifi_ap[20];
extern int g_wifi_rssi, g_wifi_ch;
extern char g_wifi_target[33];
extern char g_mach_host[64];
extern char g_wj_ssid[33];
extern int g_wj_state;
extern char g_pw[65];
extern bool g_pw_show;
extern bool g_shift;
extern bool g_caps;
extern int g_layer;
extern lv_obj_t *g_fg_btn, *g_fg_lbl;
void wifi_refresh(void);
void wifi_page_shown(int k);
void open_address_page(void);
void on_addr_row(lv_event_t *e);
void on_forget(lv_event_t *e);
void forget_refresh(void);
void build_wifi(lv_obj_t *m);

// ---- screens/history.cpp: the shot history page ---------------------------------------------------------------------
extern bd_history *g_hx;
extern bool g_hx_ok;
void hist_page_shown(void);
void hist_timer(void);
void build_hist(lv_obj_t *m);
