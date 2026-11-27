// A core that does nothing, for the render harness (sim/harness.cpp drives ui.cpp's screens directly and never
// sends anything): every command is refused as offline.
#include <stdarg.h>
#include <stdlib.h>
#include "brewdeck_core.h"
bd_result bd_select_profile(int, bool) { return BD_ERR_OFFLINE; }
bd_result bd_set_stop_weight(int, float, bool) { return BD_ERR_OFFLINE; }
bd_result bd_set_brew_temp(int, int, bool) { return BD_ERR_OFFLINE; }
bd_result bd_flush_mode(bd_flush) { return BD_ERR_OFFLINE; }
bd_result bd_tare(void) { return BD_ERR_OFFLINE; }
bd_result bd_load_previous_shot(const char *, int) { return BD_ERR_OFFLINE; }
bd_result bd_load_stored_shot(uint32_t) { return BD_ERR_OFFLINE; }
void bd_wifi_scan(void) {}
void bd_wifi_join(const char *, const char *) {}
void bd_wifi_forget(void) {}
void bd_wifi_retry(void) {}
void bd_set_machine_address(const char *) {}
void bd_set_backlight(int) {}
const bd_release *bd_release_info(void) { static bd_release r; return &r; }
uint32_t bd_seq_release(void) { return 0; }
void bd_release_check(void) {}
bd_result bd_release_install(void) { return BD_ERR_UNKNOWN_ID; }
void bd_display_stats(uint32_t *us, uint32_t *px, bool) { if (us) *us = 0; if (px) *px = 0; }
void bd_debug_route(const char *, bd_debug_fn) {}
const bd_core_info *bd_core(void) { static bd_core_info i; return &i; }   // no version, no build: the renders stay fixed
void bd_log(const char *, ...) {}
void *bd_alloc(size_t n, bd_mem) { return malloc(n); }
void bd_free(void *p) { free(p); }
static bd_profiles g_none;                       // no shot history here: no profile has a last_used
const bd_profiles *bd_profile_list(void) { return &g_none; }
