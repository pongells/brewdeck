// Brewdeck's screens (LVGL 9): fed through ui_set_* by ui_bridge.cpp; commands go to the core with bd_* directly.
#pragma once

#include "../core/brewdeck_core.h"

#ifdef __cplusplus
extern "C" {
#endif

void ui_init(void);
// the boot splash (Brewdeck, unofficial): 5 s or until a tap; called at start when the setting is on
void ui_splash(void);
void ui_set_live(const bd_machine *m);           // machine state and link
void ui_set_profiles(const bd_profiles *l);      // list for the profile picker (display only, never selects)
void ui_set_shape(const bd_profile_shape *s);    // active profile: preview graph + stop weight (display only)
void ui_shot_start(void);                        // a shot began (BD_EV_SHOT_START): its first point opens brewing
void ui_shot(const bd_shot_point *p);            // each shot point in order: feeds the brewing screen
void ui_shot_end(void);                          // the shot ended (BD_EV_SHOT_END): "Done", then the review
// each profile's original values (the UI's storage): "edited" marks, "Reset to ..." in the keypad, Restore on the
// Profiles page. load: false if none kept; save: keep these as the profile's original
void ui_on_originals(bool (*load)(const char *profile, int *w10, int *deg),
                     void (*save)(const char *profile, int w10, int deg));
// when each profile last became active (the UI's storage, one blob): load fills buf, returns the stored size (0 = none)
void ui_on_picks(size_t (*load)(void *buf, size_t cap), void (*save)(const void *buf, size_t n));
void ui_set_clock(int h, int m);                          // local time for the idle screen (-1: not known yet)
// the menu changed a screen setting (to be saved). bounce: the idle clock bounces around instead of standing still;
// splash: show the boot splash at start
void ui_on_settings(void (*cb)(int bright, int idle_min, bool bounce, bool splash));
void ui_set_settings(int bright, int idle_min, bool bounce, bool splash);   // saved settings at boot
void ui_set_wifi(const char *ssid, int rssi, int channel, const char *ap);   // menu: Wi-Fi page
void ui_set_sysinfo(const char *build, const char *version, const char *uptime, const char *memory);   // About page
void ui_set_wifi_scan(const bd_wifi_net *n, int count, bool scanning);
void ui_set_wifi_join(int state);                         // bd_join
void ui_set_update(int state, int pct, uint32_t got, uint32_t total, const char *built, const char *from,
                   const char *version);   // BD_EV_UPDATE
void ui_set_offline_reason(int why, const char *ip);     // bd_why: why the link is down
void ui_set_wifi_target(const char *ssid);                // network the screen tries to join ("" = none set up)
void ui_set_machine_host(const char *host);               // configured machine address (shown, prefilled)
void ui_set_addr(const char *screen_ip, const char *machine);   // shown on the offline screen
void ui_set_prev(int state, const bd_shot *t);  // previous shot: bd_load (BD_LOAD_*); t is copied when BD_LOAD_OK
// shot history (menu): the machine's index of stored shots; a tap asks the core for one, the answer opens the review
void ui_set_shot_index(const bd_history *ix);
// machine notification (an ordinary one): banner for ~5 s, wakes the screen; type BD_NOTICE_* (brewdeck_core.h)
void ui_notify(int type, const char *msg);
// the pressure-release glow, applied to a finished area just before it goes to the panel
void ui_flush_filter(int x1, int y1, int x2, int y2, uint16_t *px, int stride);
void ui_command_answer(int cmd, int answer);       // BD_EV_COMMAND (bd_command, bd_answer): not taken -> revert
void ui_pressure_release(int count);              // countdown beat N (>= 0) or releasing (-1): glow + banner
void ui_update_notice(int type, const char *release);   // the machine's update notice (release name, "" if not given)
// Brewdeck's own releases (bd_release_info): Menu > About "Brewdeck version" row + Install, one notice per new version
void ui_set_release(int state, const char *version, bool installing, const char *error);
// a stored shot asked for from the shot history (bd_load states): opens the review on it
void ui_set_hist(int state, const bd_shot *s, const bd_profile_shape *sh, const bd_shot *prev, int prev_state);

// hooks for the desktop render test, the emulator and the debug benchmark (harmless on the device)
void ui_test_open_editor(void);
void ui_test_open_temp_editor(void);
void ui_test_key(char key);       // '0'-'9', '.', or 'b' for backspace
void ui_test_done(void);
void ui_test_cancel(void);
void ui_test_step(int tenths);    // +/- target weight in tenths of a gram
void ui_test_cursor(int on);
void ui_test_profiles(int open, int all);   // open / close the profile picker; all: ignored (one list)
void ui_test_idle(void);          // go to the idle screen now (the next touch wakes it)
void ui_test_wake(void);         // like a touch on the idle clock
void ui_test_screen_off(void);   // screen off now
void ui_test_splash_end(void);    // close the boot splash now (the desktop emulator starts without it)
bool ui_test_splash_shown(void);  // the boot splash is on screen (the web demo holds it until the visitor starts)
int  ui_test_brew_shown(void);    // 0 no shot screen, 1 brewing, 2 done or the review (the web demo shows a colour key)
const char *ui_bench_show(int i);   // debug benchmark: show screen i (0..), NULL past the last
void ui_bench_glow(bool on);        // debug benchmark: the pressure-release glow held at 60 % / off
void ui_test_home(void);          // close menu, picker, editor, idle, brewing/review: back to Ready (emulator)
void ui_test_wifi_password(const char *ssid);   // menu > Wi-Fi password page for this network
void ui_test_menu(int page);      // -2 close, -1 menu list, 0 Display, 1 Scale, 2 Wi-Fi, 3 About, 4 Wi-Fi details,
                                  // 6 machine address, 7 shot history
void ui_test_review(int mode, int t10);     // shot review: 0 vs profile target, 1 vs previous shot; scrubber at t10

#ifdef __cplusplus
}
#endif
