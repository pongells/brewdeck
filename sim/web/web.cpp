// Web demo: the real ui.cpp and the emulator's fake machine (sim/emu.cpp, recordings from sim/data) compiled to
// WebAssembly. Nothing goes on the network: the page never talks to a machine. The page (index.html) drives it:
// web_frame() once per animation frame, the canvas reads the 800x480 RGB565 frame buffer when web_frame() says it
// changed, pointer events go to web_touch(), the HTML buttons to web_action().
// Build: sim/web/build.sh (needs Emscripten: brew install emscripten).
#define EMU_WEB 1
#include "../emu.cpp"

static bool g_hold_splash = true;                 // until web_start(): the splash's own 5 s timer must not end it

extern "C" {
EMSCRIPTEN_KEEPALIVE void web_init(void) {
  bd_ui_init();                                  // our UI on the emulator's board layer and fake core (emu.cpp);
  g_dev_disp = lv_display_get_default();            // it shows the splash, as the screen does at start
  emu_setup();
}
// one loop pass; 1 = the frame buffer changed since the last call
EMSCRIPTEN_KEEPALIVE int web_frame(void) {
  if (g_hold_splash) lv_display_trigger_activity(g_dev_disp);   // waiting to start is not idle: no dimming, no screen off
  emu_step(SDL_GetTicks());
  if (g_hold_splash && !ui_test_splash_shown()) {
    // its 5 s timer closed it and this pass drew the screen under it: put it back and keep that frame from the page
    // (the canvas keeps the splash; LVGL redraws the whole splash on a later pass, which is then shown)
    ui_splash();
    g_dirty = false;
    return 0;
  }
  const bool d = g_dirty;
  g_dirty = false;
  return d;
}
EMSCRIPTEN_KEEPALIVE uint16_t *web_fb(void) { return g_fb; }
EMSCRIPTEN_KEEPALIVE int web_backlight(void) { return g_backlight; }
EMSCRIPTEN_KEEPALIVE void web_touch(int x, int y, int down) {
  g_mx = x < 0 ? 0 : x >= W ? W - 1 : x;
  g_my = y < 0 ? 0 : y >= H ? H - 1 : y;
  g_mdown = down != 0;
}
EMSCRIPTEN_KEEPALIVE int web_action(const char *name) { return run_named(name); }
EMSCRIPTEN_KEEPALIVE const char *web_status(void) { return g_status_txt; }
EMSCRIPTEN_KEEPALIVE int web_shot_playing(void) { return g_shot_on; }
// how far the recorded shot has played, in ms of shot time (the review fast-forward runs it at 10x)
EMSCRIPTEN_KEEPALIVE int web_shot_ms(void) { return g_shot_on ? (int)((SDL_GetTicks() - g_shot_t0) * (uint32_t)g_speed) : 0; }
EMSCRIPTEN_KEEPALIVE int web_brew_shown(void) { return ui_test_brew_shown(); }   // 0 none, 1 brewing, 2 done / review
EMSCRIPTEN_KEEPALIVE int web_steam_speed(void) { return steam_speed(); }   // 0 none, 1, 10
EMSCRIPTEN_KEEPALIVE int web_splash(void) { return ui_test_splash_shown(); }
// the page opens on the splash (the name and "Unofficial ...") and keeps it until the visitor starts the demo
EMSCRIPTEN_KEEPALIVE void web_start(void) {
  g_hold_splash = false;
  if (ui_test_splash_shown()) ui_test_splash_end();
}
}

int main(void) {
  web_init();
  return 0;                                      // the runtime stays (EXIT_RUNTIME=0); the page calls web_frame()
}
