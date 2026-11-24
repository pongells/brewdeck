// The updating page: a firmware update arriving over Wi-Fi (BD_EV_UPDATE, debug builds).
// Same positions as the offline page: the logo in the 112 px tile with a badge (blue arrow while receiving, green check
// when installed, orange "!" when it stopped), title, text, a progress bar where the offline page has its buttons,
// the new build's date and the sender on the footer line. Top layer, above everything; no buttons (unplugging keeps
// the old version). Installed: the core restarts ~2 s later. Stopped: back to the screen below after 5 s.
#include "../ui_int.h"

static const int UP_TILE_X = 344, UP_TILE_Y = 72, UP_TILE = 112, BAR_X = 220, BAR_Y = 340, BAR_W = 360;
static const uint32_t STOPPED_MS = 5000;
static lv_obj_t *g_up, *g_up_dot, *g_up_sym, *g_up_title, *g_up_body, *g_up_fill, *g_up_amount, *g_up_foot;
static int g_up_state = BD_UPDATE_NONE;
static uint32_t g_up_stopped_at = 0;
static int g_up_pct = 0;

// the badge's glyph: an arrow down (receiving) or a check (installed), 2.4 px lines on the 30 px dot
static void up_glyph_draw(lv_event_t *e) {
  if (g_up_state != BD_UPDATE_RECEIVING && g_up_state != BD_UPDATE_INSTALLED) return;
  lv_layer_t *ctx = lv_event_get_layer(e);
  lv_area_t a;
  lv_obj_get_coords(lv_event_get_target_obj(e), &a);
  lv_draw_line_dsc_t l;
  lv_draw_line_dsc_init(&l);
  l.color = COL_BG;
  l.width = 3;
  l.round_start = l.round_end = 1;
  const int x = a.x1 + 7, y = a.y1 + 7;   // a 16 px glyph in the middle of the 30 px dot
  auto seg = [&](float x0, float y0, float x1, float y1) {
    const lv_point_t p0 = {(int32_t)(x + x0 + 0.5f), (int32_t)(y + y0 + 0.5f)};
    const lv_point_t p1 = {(int32_t)(x + x1 + 0.5f), (int32_t)(y + y1 + 0.5f)};
    dl(ctx, &l, &p0, &p1);
  };
  if (g_up_state == BD_UPDATE_RECEIVING) {
    seg(8, 2.5f, 8, 11.5f);
    seg(4, 8, 8, 12);
    seg(12, 8, 8, 12);
  } else {
    seg(3.5f, 8.5f, 6.5f, 11.5f);
    seg(6.5f, 11.5f, 12.5f, 4.5f);
  }
}

static void up_show(void) {
  if (!lv_obj_is_hidden(g_up)) return;
  ui_screen_wake();                         // lit, the screen off and the dimmed clock out of the way
  lv_obj_set_hidden(g_up, false);
  lv_obj_move_to_index(g_up, -1);           // above a notice banner too
}

static void on_up_timer(lv_timer_t *t) {
  (void)t;
  if (g_up_state != BD_UPDATE_STOPPED || lv_obj_is_hidden(g_up)) return;
  const uint32_t el = lv_tick_elaps(g_up_stopped_at);
  if (el >= STOPPED_MS) {
    g_up_state = BD_UPDATE_NONE;
    lv_obj_set_hidden(g_up, true);
    return;
  }
  char b[64];
  snprintf(b, sizeof b, "Stopped at %d %% \xC2\xB7 back in 0:%02u", g_up_pct, (unsigned)((STOPPED_MS - el + 999) / 1000));
  set_text(g_up_amount, b);
}

void ui_set_update(int state, int pct, uint32_t got, uint32_t total, const char *built, const char *from) {
  if (state == BD_UPDATE_NONE) {
    g_up_state = state;
    lv_obj_set_hidden(g_up, true);
    return;
  }
  if (pct < 0) pct = 0;
  if (pct > 100) pct = 100;
  g_up_pct = pct;
  lv_display_trigger_activity(NULL);        // an update counts as activity: no dimming, no screen off meanwhile
  const bool changed = state != g_up_state;
  g_up_state = state;
  up_show();
  char b[96];
  if (changed) {
    const lv_color_t dot = state == BD_UPDATE_RECEIVING ? COL_BLUE : state == BD_UPDATE_INSTALLED ? COL_GREEN : COL_WARN;
    set_bg(g_up_dot, dot);
    set_hidden(g_up_sym, state != BD_UPDATE_STOPPED);
    lv_obj_invalidate(g_up_dot);
    set_text(g_up_title, state == BD_UPDATE_RECEIVING ? "Updating the screen"
                         : state == BD_UPDATE_INSTALLED ? "Update installed" : "Update stopped");
    set_text(g_up_body,
             state == BD_UPDATE_RECEIVING ? "New Brewdeck firmware is arriving over Wi-Fi. Leave the screen on: the machine works as usual."
             : state == BD_UPDATE_INSTALLED ? "Restarting now. If the new version does not run properly, the screen goes back to this one by itself."
                                            : "The update did not arrive completely. Nothing changed: the screen keeps its current version.");
    set_bg(g_up_fill, state == BD_UPDATE_RECEIVING ? COL_BLUE : state == BD_UPDATE_INSTALLED ? COL_GREEN : COL_BORDER);
    if (built && built[0] && from && from[0]) snprintf(b, sizeof b, "New version built %s \xC2\xB7 sent from %s", built, from);
    else if (built && built[0]) snprintf(b, sizeof b, "New version built %s", built);
    else snprintf(b, sizeof b, "%s", from && from[0] ? from : "");
    set_text(g_up_foot, b);
    if (state == BD_UPDATE_STOPPED) g_up_stopped_at = stamp_nz(lv_tick_get());
    bd_log("ui: update page: %s", state == BD_UPDATE_RECEIVING ? "receiving" : state == BD_UPDATE_INSTALLED ? "installed" : "stopped");
  }
  const int w = state == BD_UPDATE_INSTALLED ? BAR_W : BAR_W * pct / 100;
  lv_obj_set_width(g_up_fill, w < 8 ? 8 : w);   // at least its round ends
  if (state == BD_UPDATE_RECEIVING)
    snprintf(b, sizeof b, "%d %% \xC2\xB7 %.1f of %.1f MB", pct, got / 1048576.0f, total / 1048576.0f);
  else if (state == BD_UPDATE_INSTALLED) snprintf(b, sizeof b, "100 %% \xC2\xB7 restarting...");
  else snprintf(b, sizeof b, "Stopped at %d %% \xC2\xB7 back in 0:%02u", pct, (unsigned)(STOPPED_MS / 1000));
  set_text(g_up_amount, b);
}

static lv_obj_t *centred(lv_obj_t *p, const lv_font_t *f, lv_color_t c, int y) {
  lv_obj_t *l = label(p, "", f, c);
  lv_obj_set_width(l, 800);
  lv_obj_set_style_text_align(l, LV_TEXT_ALIGN_CENTER, 0);
  lv_obj_set_pos(l, 0, y);
  return l;
}

void build_update(void) {
  g_up = box(lv_layer_top(), 0, 0, 800, 480, COL_BG, 0);
  tag(g_up, "screen.update");
  lv_obj_set_clickable(g_up, true);         // swallow taps: nothing under it reacts meanwhile
  lv_obj_t *tile = box(g_up, UP_TILE_X, UP_TILE_Y, UP_TILE, UP_TILE, COL_CARD, 26);
  lv_obj_set_pos(logo(tile, UP_TILE), 0, 0);   // the logo's own card square is the tile's colour
  lv_obj_t *badge = box(g_up, UP_TILE_X + UP_TILE - 28, UP_TILE_Y - 8, 36, 36, COL_BG, 18);   // 3 px ring of background
  g_up_dot = box(badge, 3, 3, 30, 30, COL_BLUE, 15);
  lv_obj_add_event_cb(g_up_dot, up_glyph_draw, LV_EVENT_DRAW_MAIN_END, NULL);
  g_up_sym = label(g_up_dot, "!", F_TEXT_B, COL_BG);
  lv_obj_center(g_up_sym);
  g_up_title = centred(g_up, F_TITLE, COL_TEXT, 211);
  tag(g_up_title, "value.update_title");
  g_up_body = label(g_up, "", F_TEXT, COL_MUTED);
  lv_label_set_long_mode(g_up_body, LV_LABEL_LONG_MODE_WRAP);
  lv_obj_set_width(g_up_body, 560);
  lv_obj_set_style_text_align(g_up_body, LV_TEXT_ALIGN_CENTER, 0);
  lv_obj_set_style_text_line_space(g_up_body, 2, 0);   // 22 px lines, as the offline page
  lv_obj_set_pos(g_up_body, 120, 257);
  lv_obj_t *track = box(g_up, BAR_X, BAR_Y, BAR_W, 8, COL_CARD, 4);
  g_up_fill = box(track, 0, 0, 8, 8, COL_BLUE, 4);
  g_up_amount = centred(g_up, F_TEXT, COL_MUTED, 362);
  tag(g_up_amount, "value.update_progress");
  g_up_foot = centred(g_up, F_TEXT, COL_OUTLINE, 430);
  lv_obj_set_hidden(g_up, true);
  lv_timer_create(on_up_timer, 250, NULL);
}
