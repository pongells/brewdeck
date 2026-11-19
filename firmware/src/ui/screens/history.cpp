// Shot history (menu page): the stored shots, opened in the shot review.
#include "../ui_int.h"

// ---- Shot history (menu page 7) ----------------------------------------------------------
// The newest 50 stored shots from the machine's WebSocket index (no request): day headers, then time, profile and
// duration per row (grams once that shot was opened in this session). A tap loads the shot and the next older one of
// its profile (bd_load_stored_shot, sent once the machine is idle) and opens the shot review on it.
WsShotIndex *g_hx;                         // the machine's index of stored shots (PSRAM)
bool g_hx_ok = false;                      // g_hx holds an index
static lv_obj_t *g_hl_list;
static EmptyState g_hl_empty;
static uint32_t g_hl_ids[WS_SHOT_INDEX_MAX];
static lv_obj_t *g_hl_val[WS_SHOT_INDEX_MAX];
static int g_hl_n = 0;
static uint32_t g_hl_wait = 0, g_hl_wait_ms = 0;  // shot being loaded (0 = none)
static uint32_t g_hl_msg_id = 0;                  // row showing "Not available" / "Machine busy"
static const char *g_hl_msg = "";
static struct { uint32_t id; uint16_t w10; } g_hw[WS_SHOT_INDEX_MAX];   // yield of shots opened this session
static int g_hw_n = 0;

static int hw_find(uint32_t id) { for (int i = 0; i < g_hw_n; i++) if (g_hw[i].id == id) return i; return -1; }
static void hw_put(uint32_t id, unsigned w10) {
  int i = hw_find(id);
  if (i < 0) {
    if (g_hw_n < WS_SHOT_INDEX_MAX) i = g_hw_n++;
    else { memmove(&g_hw[0], &g_hw[1], sizeof g_hw[0] * (WS_SHOT_INDEX_MAX - 1)); i = WS_SHOT_INDEX_MAX - 1; }
  }
  g_hw[i].id = id;
  g_hw[i].w10 = (uint16_t)w10;
}
// "Today" / "Yesterday" / "4 Nov" (+ year when not this one); "Date unknown" while a clock is not set
static void fmt_day(uint32_t ts, char *b, size_t n) {
  const time_t now = time(NULL), t = (time_t)ts;
  if (ts < 1600000000u || now < 1600000000) { snprintf(b, n, "Date unknown"); return; }
  struct tm a, c;
  localtime_r(&t, &a);
  localtime_r(&now, &c);
  static const char *const MON[12] = {"Jan", "Feb", "Mar", "Apr", "May", "Jun", "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"};
  if (a.tm_year == c.tm_year && a.tm_yday == c.tm_yday) snprintf(b, n, "Today");
  else if (a.tm_year == c.tm_year && a.tm_yday + 1 == c.tm_yday) snprintf(b, n, "Yesterday");
  else if (a.tm_year == c.tm_year) snprintf(b, n, "%d %s", a.tm_mday, MON[a.tm_mon % 12]);
  else snprintf(b, n, "%d %s %d", a.tm_mday, MON[a.tm_mon % 12], a.tm_year + 1900);
}
static void hist_row_value(lv_obj_t *l, const WsShotRec *r) {
  char b[40];
  if (r->id == g_hl_wait) { set_text(l, "Loading..."); set_tc(l, COL_MUTED); return; }
  if (r->id == g_hl_msg_id && g_hl_msg[0]) { set_text(l, g_hl_msg); set_tc(l, COL_WARN); return; }
  const int k = hw_find(r->id);
  if (k >= 0) snprintf(b, sizeof b, "%u.%u g  \xC2\xB7  %u.%u s", g_hw[k].w10 / 10u, g_hw[k].w10 % 10u,
                       (unsigned)r->dur10 / 10u, (unsigned)r->dur10 % 10u);
  else snprintf(b, sizeof b, "%u.%u s", (unsigned)r->dur10 / 10u, (unsigned)r->dur10 % 10u);
  set_text(l, b);
  set_tc(l, COL_MUTED);
}
static void hist_values(void) {
  for (int i = 0; i < g_hl_n && g_hx_ok; i++)
    for (int j = 0; j < g_hx->n; j++) if (g_hx->r[j].id == g_hl_ids[i]) { hist_row_value(g_hl_val[i], &g_hx->r[j]); break; }
}
static void on_hist_row(lv_event_t *e) {
  const uint32_t id = (uint32_t)(uintptr_t)lv_event_get_user_data(e);
  if (g_hl_wait) return;                          // one at a time
  g_hl_msg_id = 0;
  if (g_menu_link != LINK_ONLINE) { g_hl_msg_id = id; g_hl_msg = "Machine offline"; }
  else if (g_idle_busy || g_br_state != 0) { g_hl_msg_id = id; g_hl_msg = "Machine busy"; }
  else if (bd_load_stored_shot(id) == BD_OK) { g_hl_wait = id; g_hl_wait_ms = lv_tick_get(); }
  hist_values();
}
// rows are added a few at a time (all 50 at once was one ~0.6 s frame on the device). Scrolling is ~80 ms per frame
// (LVGL redraws the whole list area per 20-line strip); a custom-drawn list was tried and dropped (this is good enough)
static int g_hb_i = 0;                            // next index record to add
static char g_hb_day[24] = "";
static lv_timer_t *g_hb_timer;
static void hist_add(int k) {
  char b[48];
  for (; k > 0 && g_hx_ok && g_hb_i < g_hx->n; k--) {
    const WsShotRec *r = &g_hx->r[g_hb_i++];
    fmt_day(r->ts, b, sizeof b);
    if (strcmp(b, g_hb_day)) {                     // day header
      snprintf(g_hb_day, sizeof g_hb_day, "%s", b);
      lv_obj_t *h = box(g_hl_list, 0, 0, 728, 44, COL_BG, 0);
      lv_obj_set_style_bg_opa(h, LV_OPA_TRANSP, 0);   // the list's background is enough
      lv_obj_t *l = label(h, b, F_TEXT, COL_MUTED);
      lv_obj_align(l, LV_ALIGN_BOTTOM_LEFT, 0, -8);
    }
    lv_obj_t *row = box(g_hl_list, 0, 0, 728, 60, COL_BG, 0);
    hairline(row, LV_BORDER_SIDE_BOTTOM);
    make_button(row, COL_CARD);
    lv_obj_set_style_bg_opa(row, LV_OPA_TRANSP, 0);  // drawn only while pressed
    lv_obj_set_style_bg_opa(row, LV_OPA_COVER, LV_STATE_PRESSED);
    lv_obj_add_event_cb(row, on_hist_row, LV_EVENT_CLICKED, (void *)(uintptr_t)r->id);
    if (r->ts >= 1600000000u) {
      const time_t t = (time_t)r->ts;
      struct tm a;
      localtime_r(&t, &a);
      snprintf(b, sizeof b, "%02d:%02d", a.tm_hour, a.tm_min);
    } else snprintf(b, sizeof b, "#%u", (unsigned)r->id);
    lv_obj_t *tl = label(row, b, F_ROW, COL_TEXT);
    lv_obj_align(tl, LV_ALIGN_LEFT_MID, 0, 0);
    display_name(r->name[0] ? r->name : "Unknown profile", b, sizeof b, NULL);
    lv_obj_t *nl = label(row, b, F_ROW, COL_TEXT);
    one_line(nl, 400);
    lv_obj_align(nl, LV_ALIGN_LEFT_MID, 84, 0);
    lv_obj_t *rt = flex(row, LV_FLEX_FLOW_ROW, 12);
    lv_obj_align(rt, LV_ALIGN_RIGHT_MID, -8, 0);
    lv_obj_t *v = label(rt, "", F_TEXT, COL_MUTED);
    icon_chevron_right(rt, COL_MUTED);
    g_hl_ids[g_hl_n] = r->id;
    g_hl_val[g_hl_n++] = v;
    hist_row_value(v, r);
  }
}
static void on_hb_timer(lv_timer_t *t) {
  if (lv_obj_is_hidden(g_menu) || g_menu_cur != 7 || !g_hx_ok || g_hb_i >= g_hx->n) {
    lv_timer_pause(t);
    return;
  }
  hist_add(6);
}
// fingerprint of what the list shows (records, empty-state reason, the day for its headers): rebuilding the rows took
// ~110 ms of the page's ~190 ms opening, and usually nothing has changed since the last time
static uint32_t g_hb_sig = 0;
static uint32_t hist_sig(void) {
  uint32_t h = 2166136261u;
  auto mix = [&h](const void *p, size_t n) { for (size_t i = 0; i < n; i++) { h ^= ((const uint8_t *)p)[i]; h *= 16777619u; } };
  const int n = g_hx_ok ? g_hx->n : -1;
  mix(&n, sizeof n);
  for (int i = 0; i < n; i++) { mix(&g_hx->r[i].id, 4); mix(&g_hx->r[i].ts, 4); mix(g_hx->r[i].name, strlen(g_hx->r[i].name)); }
  if (n <= 0) mix(&g_menu_link, sizeof g_menu_link);
  const time_t now = time(NULL);
  struct tm c;
  localtime_r(&now, &c);
  const int day = now > 1600000000 ? c.tm_year * 400 + c.tm_yday : -1;
  mix(&day, sizeof day);
  return h;
}
static void hist_build(void) {
  g_hb_sig = hist_sig();
  lv_obj_clean(g_hl_list);
  g_hl_n = 0;
  g_hb_i = 0;
  g_hb_day[0] = 0;
  const int n = g_hx_ok ? g_hx->n : 0;
  set_hidden(g_hl_empty.box, n > 0);
  if (g_hx_ok) empty_set(g_hl_empty, "No stored shots", "Shots the machine keeps appear here after you pull one.");
  else if (g_menu_link == LINK_ONLINE) empty_set(g_hl_empty, "Loading the list", "It comes from the machine.");
  else empty_set(g_hl_empty, "Machine not connected", "The list comes from the machine.");
  hist_add(8);                                     // what fits on the page (+1), the rest from the timer
  if (g_hb_i < n) lv_timer_resume(g_hb_timer);
}
void hist_page_shown(void) {
  g_hl_msg_id = 0;
  if (hist_sig() != g_hb_sig) hist_build();
  else {                                           // the rows are still right: only their values (grams, notes)
    hist_values();
    if (g_hx_ok && g_hb_i < g_hx->n) lv_timer_resume(g_hb_timer);
  }
  lv_obj_scroll_to_y(g_hl_list, 0, LV_ANIM_OFF);
}
void hist_timer(void) {                    // from menu_timer (1 s): a load that never answered
  if (g_hl_wait && lv_tick_elaps(g_hl_wait_ms) > 15000) {
    g_hl_msg_id = g_hl_wait; g_hl_msg = "Not available"; g_hl_wait = 0;
    if (g_menu_cur == 7) hist_values();
  }
}
void ui_set_shot_index(const WsShotIndex *ix) {
  if (!g_hx) return;
  *g_hx = *ix;
  g_hx_ok = true;
  if (g_menu_cur == 7 && !lv_obj_is_hidden(g_menu)) hist_build();
}
void ui_set_hist(int state, const ShotTrace *s, const ProfileShape *sh, const ShotTrace *prev, int prev_state) {
  if (!g_hl_wait || state == RV_PREV_LOADING || state == RV_PREV_NONE) return;
  const uint32_t id = g_hl_wait;
  g_hl_wait = 0;
  if (state != RV_PREV_READY || !s || (uint32_t)s->id != id || s->n < 3) {
    g_hl_msg_id = id; g_hl_msg = "Not available";
    if (g_menu_cur == 7) hist_values();
    return;
  }
  hw_put(id, s->pt[s->n - 1].w);
  if (g_menu_cur == 7) hist_values();
  // still on the list (not closed, no shot started meanwhile): open it
  if (g_menu_cur == 7 && !lv_obj_is_hidden(g_menu) && g_br_state == 0) rv_open_stored(s, sh, prev, prev_state);
}
void build_hist(lv_obj_t *m) {
  lv_obj_t *pg = g_menu_page[7] = box(m, 24, 96, 752, 384, COL_BG, 0);
  g_hl_list = box(pg, 0, 0, 752, 384, COL_BG, 0);
  lv_obj_set_flex_flow(g_hl_list, LV_FLEX_FLOW_COLUMN);
  lv_obj_set_clickable(g_hl_list, true);
  lv_obj_set_scrollable(g_hl_list, true);
  lv_obj_set_scroll_dir(g_hl_list, LV_DIR_VER);
  lv_obj_set_style_pad_bottom(g_hl_list, 24, 0);
  lv_obj_set_style_width(g_hl_list, 4, LV_PART_SCROLLBAR);
  lv_obj_set_style_radius(g_hl_list, 2, LV_PART_SCROLLBAR);
  lv_obj_set_style_bg_color(g_hl_list, COL_OUTLINE, LV_PART_SCROLLBAR);
  lv_obj_set_style_bg_opa(g_hl_list, LV_OPA_COVER, LV_PART_SCROLLBAR);
  g_hl_empty = empty_state(pg, IC_HISTORY);
  g_hx = (WsShotIndex *)bd_alloc(sizeof(WsShotIndex), BD_MEM_PSRAM);   // ~2.8 KB
  if (g_hx) memset(g_hx, 0, sizeof *g_hx);
  g_hb_timer = lv_timer_create(on_hb_timer, 30, NULL);
  lv_timer_pause(g_hb_timer);
  set_hidden(pg, true);
}
