// Web demo: the real ui.cpp and the emulator's fake machine (sim/emu.cpp, recordings from sim/data) compiled to
// WebAssembly. Nothing goes on the network: the page never talks to a machine. The page (index.html) drives it:
// web_frame() once per animation frame, the canvas reads the 800x480 RGB565 frame buffer when web_frame() says it
// changed, pointer events go to web_touch(), the HTML buttons to web_action().
// Build: sim/web/build.sh (needs Emscripten: brew install emscripten).
#define EMU_WEB 1
#include "../emu.cpp"

extern "C" {
EMSCRIPTEN_KEEPALIVE void web_init(void) {
  bd_ui_init();                                  // our UI on the emulator's board layer and fake core (emu.cpp);
  g_dev_disp = lv_display_get_default();            // it shows the splash, as the screen does at start
  emu_setup();
}
// one loop pass; 1 = the frame buffer changed since the last call
EMSCRIPTEN_KEEPALIVE int web_frame(void) {
  emu_step(SDL_GetTicks());
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
EMSCRIPTEN_KEEPALIVE int web_steam_on(void) { return g_m.steam_active || g_st.on; }
}

int main(void) {
  web_init();
  return 0;                                      // the runtime stays (EXIT_RUNTIME=0); the page calls web_frame()
}
