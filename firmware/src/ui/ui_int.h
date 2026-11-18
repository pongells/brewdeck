#pragma once
// Internal to Brewdeck's UI (ui.cpp + screens/*.cpp): what the screens share. The public side is ui.h.
#include <ctype.h>
#include <stdint.h>
// A "since" timestamp where 0 means "not set". Never write `now | 1`: on an even tick that is 1 ms in the future, so an
// elapsed check in the same millisecond wraps to ~49 days and passes at once. 0 maps to UINT32_MAX = 1 ms in the past.
static inline uint32_t stamp_nz(uint32_t now) { return now ? now : 0xFFFFFFFFu; }
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

static inline void Serial_log(const char *m) { bd_log("%s", m); }   // the core's log (time in front on the device)

// ---- constants and macros
// quiet axes on the brewing / steam charts: hairline grid + small grey labels
#define COL_GRID lv_color_hex(0x161618)
// with the Display setting "Idle clock: Bouncing" (off by default) the clock block bounces around
// the screen like the old DVD logo (for fun; it also does the job of the anti-retention shift used when it stands still). 1 px per 40 ms on each axis (25 px/s, 45 degrees); each wall
// it hits gives the clock the next muted tint (all as dim as the grey, it is a night screen). Only the block's
// strip is redrawn per step (~350x190 px).
#define IDLE_BOUNCE_MS 40
#define IDLE_EDGE 24                  // the screens' margin
// After 30 min without a touch (or the "Dim after" time if longer; not when dimming is Off) and nothing running, or
// Menu: one column of rows that open a page; profiles and maintenance are left to the machine's web UI (one line under
// the list). Display: this screen's settings (saved by ui_bridge.cpp through ui_on_settings). Scale / Wi-Fi / About:
// status pages.
#define MENU_PAGES 8   // 0 Display, 1 Scale, 2 Wi-Fi list, 3 About, 4 Wi-Fi details, 5 password, 6 machine address (= 5's objects), 7 shot history

// ---- types and shared constants
static_assert(sizeof(lv_point_precise_t) == sizeof(lv_point_t), "point layout (LV_USE_FLOAT 0)");
static const int W_MIN10 = 50, W_MAX10 = 800, W_STEP10 = 5;
// a stop weight from the machine into the range the screen edits (0 = no stop weight stays 0)
static inline int clamp_w10(int w10) { return w10 <= 0 ? 0 : w10 < W_MIN10 ? W_MIN10 : w10 > W_MAX10 ? W_MAX10 : w10; }
static const int PV_W = 344, PV_H = 100;         // profile preview box (fits the 24 px screen margin)
enum { IC_HISTORY, IC_SCALE, IC_DISPLAY, IC_WIFI, IC_ABOUT, IC_PROFILES, IC_SETTINGS, IC_BELL };
struct EmptyState { lv_obj_t *box, *title, *body; };
// -/+ on Ready change the ACTIVE profile's brew temperature by 1 degC (T_MIN..T_MAX); sent 1 s after the last tap
// with bd_set_brew_temp, only while online and idle in HOME (the core checks again). The shown value follows the
// machine's active profile, the status's target until that is known.
// brew temperature: 1-105 degC (0 = not known / heater off, never sent); a + from an unknown value starts at T_START
static const int T_MIN = 1, T_MAX = 105, T_START = 60;
static const int VALS_Y = 404;                      // brewing / steam: the values row under the chart (y 404..456)
struct Restore { int id; char name[48]; int w10, deg; uint32_t t; };   // a restore waiting for its profile (profiles.cpp)
// drop centred in a 24 px box (a 4.5 px radius drop from y 5.7 to 18)
static const lv_point_t FL_DROP[] = {{12,6},{13,6},{14,8},{16,11},{16,14},{16,16},{15,17},{14,18},{12,18},{10,18},{9,17},{8,16},{8,14},{8,11},{10,8},{11,6},{12,6}};
enum { RV_PREV_NONE = 0, RV_PREV_LOADING, RV_PREV_READY, RV_PREV_MISSING, RV_PREV_FAILED };   // = bd_load (BD_LOAD_*)
static const float PRESS_ON = 0.8f;   // bar: orange only while the pill reads above 0.8
static const uint32_t PRESS_OFF_MS = 2000;   // reads 0.8 or less this long -> off (no flicker between 0.8 and 0.9)
static const int NB_Y = 38;                        // = the status pills' y

// ---- shared state (defined in the file named above each group)
// screens/common.cpp
extern int g_weight10;
extern const char *PROFILE_NAME;
extern lv_obj_t *g_value_lbl;
extern lv_obj_t *g_editor;
extern lv_obj_t *g_entry_lbl;
extern lv_obj_t *g_cursor;
extern lv_obj_t *g_unit_lbl;
extern lv_obj_t *g_was_lbl;
extern lv_obj_t *g_pill;
extern lv_obj_t *g_pill_dot;
extern lv_obj_t *g_pill_lbl;
extern lv_obj_t *g_tank;
extern lv_obj_t *g_tank_fill;
extern lv_obj_t *g_tank_lbl;
extern lv_obj_t *g_tank_pct;
extern lv_obj_t *g_prof_lbl;
extern lv_obj_t *g_ed_prof_lbl;
extern lv_obj_t *g_temp_lbl;
extern lv_obj_t *g_scale_lbl;
extern char g_entry[8];
extern bool g_cursor_on;
extern lv_obj_t *g_pick;
extern lv_obj_t *g_pick_list;
extern lv_obj_t *g_pick_sub;
extern ProfileList g_profiles;
extern char g_cur_profile[48];
extern int g_nb_pill_max;
extern lv_obj_t *g_hdr_left;
extern int32_t g_hdr_btn_x;
extern lv_obj_t *g_menu_btn;
extern lv_obj_t *g_chart;
extern lv_obj_t *g_pv_msg;
extern lv_obj_t *g_pv_line[4][24];
extern lv_point_t g_pv_pts[4][200];
extern bool g_shape_loaded;
extern ProfileShape g_last_shape;
extern int g_pend_id;
extern char g_pend_name[48];
extern int g_shape_weight10;
extern int g_shape_id;
// screens/ready.cpp
extern bool (*g_orig_load)(const char *, int *, int *);
extern void (*g_orig_save)(const char *, int, int);
extern void (*g_orig_drop)(const char *);
extern char g_orig_name[48];
extern bool g_orig_have;
extern int g_orig_w10;
extern int g_orig_deg;
extern lv_obj_t *g_tt_lbl;
extern lv_obj_t *g_tw_lbl;
extern lv_obj_t *g_reset_btn;
extern lv_obj_t *g_reset_lbl;
extern bool g_can_select;
extern int g_tset;
extern int g_act_temp;
extern lv_timer_t *g_temp_tm;
extern lv_obj_t *g_ed_title;
extern lv_obj_t *g_key_dot;
extern lv_obj_t *g_key_dot_lbl;
// screens/profiles.cpp
extern bool g_can_tare;
extern lv_obj_t *g_tare_lbl;
extern lv_obj_t *g_sc_ic[4];
extern lv_obj_t *g_scale_name;
extern lv_obj_t *g_scale_pill;
extern lv_obj_t *g_pr_pill;
extern lv_obj_t *g_pr_ic[2];
extern lv_obj_t *g_pr_name;
extern lv_obj_t *g_pr_lbl;
extern lv_timer_t *g_weight_tm;
extern int g_act_weight10;
extern Restore g_rst;
extern lv_obj_t *g_fl_btn;
extern lv_obj_t *g_fl_lbl;
extern lv_obj_t *g_fl_drop;
extern lv_obj_t *g_fl_fill;
extern int g_fl_mode;
extern bool g_fl_idle;
extern int g_fl_want;
// screens/steam.cpp
extern lv_obj_t *g_st;
// screens/brew.cpp
extern ShotPoint *g_br;
extern int g_br_n;
extern int g_br_state;
extern bool g_br_seen_brew;
extern bool g_live_brew;
extern lv_obj_t *g_brew;
extern int g_rv_prev_state;
extern int g_rv_mode;
extern int g_rv_i;
extern lv_obj_t *g_rv;
extern lv_obj_t *g_rv_chart;
// screens/offline.cpp
extern lv_obj_t *g_off;
extern int g_off_link;
extern uint32_t g_off_since;
extern uint32_t g_off_phase;
extern bool g_off_ever_online;
extern bool g_off_active;
extern char g_off_sip[20];
extern char g_off_mach[40];
extern char g_wifi_ssid[33];
extern char g_wifi_ap[20];
extern char g_wifi_target[33];
extern char g_mach_host[64];
extern bool g_in_wifi_pages;
extern int g_menu_cur;
extern int g_menu_entry;
// screens/idle.cpp
extern int g_idle_min;
extern int g_bright;
extern int g_idle_bright;
extern lv_obj_t *g_idle;
extern int g_clock_h;
extern int g_clock_m;
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
// screens/notices.cpp
extern lv_obj_t *g_menu;
extern lv_obj_t *g_nb;
extern lv_obj_t *g_nb_dot;
extern lv_obj_t *g_nb_lbl;
extern char g_upd_rel[32];
extern bool g_upd_known;
extern bool g_upd_shown;
// screens/menu.cpp
extern lv_obj_t *g_menu_title;
extern lv_obj_t *g_menu_back;
extern lv_obj_t *g_menu_main;
extern lv_obj_t *g_menu_page[8];
extern lv_obj_t *g_mrow_val[7];
extern lv_obj_t *g_info[8][7];
extern lv_obj_t *g_disp_val[2];
extern LiveData g_menu_live;
extern int g_menu_link;
extern int g_wifi_rssi;
extern int g_wifi_ch;
extern WsShotIndex *g_hx;
extern bool g_hx_ok;
extern char g_wj_ssid[33];
// screens/wifi.cpp
extern int g_wj_state;
extern char g_pw[65];
extern bool g_pw_show;
extern bool g_shift;
extern bool g_caps;
extern int g_layer;
extern lv_obj_t *g_fg_btn;
extern lv_obj_t *g_fg_lbl;

// ---- shared functions
// screens/common.cpp
void one_line(lv_obj_t *l, int32_t w);
void dl(lv_layer_t *L, lv_draw_line_dsc_t *d, const lv_point_t *a, const lv_point_t *b);
void darc(lv_layer_t *L, lv_draw_arc_dsc_t *d, const lv_point_t *c, int r, int a0, int a1);
void dlabel(lv_layer_t *L, lv_draw_label_dsc_t *d, const lv_area_t *a, const char *t);
int prof_id(const char *name);
lv_obj_t *box(lv_obj_t *p, int x, int y, int w, int h, lv_color_t bg, int r);
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
void set_w(lv_obj_t *o, int32_t w);
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
// screens/ready.cpp
void format_weight(char *out, size_t n, int tenths);
void refresh_value(void);
void step_weight(int delta10);
void refresh_tset(void);
void temp_commit(lv_timer_t *t);
void editor_open(bool temp);
void editor_close(void);
void editor_key(char k);
void editor_done(void);
void on_reset(lv_event_t *e);
void on_cancel(lv_event_t *e);
void on_done(lv_event_t *e);
void on_key(lv_event_t *e);
void on_blink(lv_timer_t *t);
void pv_loading(void);
void show_profile(const char *raw);
void foot_pill(lv_obj_t *p, bool warn, int32_t pl, int32_t pr);
void stepper_part(lv_obj_t *o);
void build_ready(lv_obj_t *scr);
// screens/profiles.cpp
void picker_close(void);
void picker_open(void);
void on_pick_open(lv_event_t *e);
void weight_changed(bool now);
void orig_sync(void);
void orig_note(void);
void weight_commit(lv_timer_t *t);   // send a waiting stop-weight change now (NULL from code)
bool orig_temp_edited(void);
bool orig_weight_edited(void);
void orig_paint(void);
void snap_apply(void);
void on_tare(lv_event_t *e);
void fl_show(void);
void on_flush_press(lv_event_t *e);
void on_flush_tap(lv_event_t *e);
void on_flush_hold(lv_event_t *e);
void on_fl_timer(lv_timer_t *t);
void picker_rebuild(void);
void build_picker(lv_obj_t *scr);
// screens/steam.cpp
void axis_time(lv_layer_t *ctx, int x0, int w, int y, int span_s, int step_s);
lv_obj_t *strip_value(lv_obj_t *row, lv_color_t dot, const char *unit);
void st_update(const LiveData *d, bool on);
void build_steam(lv_obj_t *scr);
void build_editor(lv_obj_t *scr);
// screens/brew.cpp
void ch_hline(lv_layer_t *ctx, int x1, int x2, int y, lv_color_t col);
void ch_text_c(lv_layer_t *ctx, int x, int y, const char *s, lv_text_align_t align, lv_color_t col);
void br_hide(void);
void fmt_when(uint32_t ts, char *b, size_t n);
void rv_values(void);
void rv_axis(void);
void rv_toggle_style(void);
void rv_open_stored(const ShotTrace *s, const ProfileShape *sh, const ShotTrace *prev, int prev_state);
lv_obj_t *pill_row(lv_obj_t *p);
void build_brew(lv_obj_t *scr);
// screens/offline.cpp
void on_off_timer(lv_timer_t *t);
void build_offline(lv_obj_t *scr);
// screens/idle.cpp
void bl_set(int pct);
void idle_hide(void);
void on_idle_timer(lv_timer_t *t);
void build_blank(lv_obj_t *scr);
void build_notif(void);
void build_idle(lv_obj_t *scr);
// screens/notices.cpp
void on_nb_timer(lv_timer_t *t);
void build_frame(void);
void ntc_build_btn(lv_obj_t *scr, int32_t x);
// screens/menu.cpp
uint32_t prof_last_used(const char *name);
void menu_show_page(int k);
void on_menu_open(lv_event_t *e);
void open_wifi_menu(void);
void menu_close(void);
void on_menu_close(lv_event_t *e);
void on_menu_back(lv_event_t *e);
void on_menu_row(lv_event_t *e);
void menu_timer(lv_timer_t *t);
void disp_toggle_row(lv_obj_t *c, int t, const char *title, const char *a, const char *b);
lv_obj_t *menu_row(lv_obj_t *col, const char *name, lv_obj_t **val, bool opens, bool grey);
lv_obj_t *menu_column(lv_obj_t *p, int x);
lv_obj_t *small_btn(lv_obj_t *p, bool plus, int k);
// screens/wifi.cpp
void wifi_refresh(void);
void wifi_page_shown(int k);
void open_address_page(void);
void on_addr_row(lv_event_t *e);
void on_forget(lv_event_t *e);
void forget_refresh(void);
void build_wifi(lv_obj_t *m);
// screens/history.cpp
void hist_page_shown(void);
void hist_timer(void);
void build_menu(lv_obj_t *scr);
void build_hist(lv_obj_t *m);   // the shot history page (history.cpp), built by build_menu
