#pragma once
// Firmware releases over Wi-Fi (bd_release_* in brewdeck_core.h). Looks for the newest release on GitHub at start and
// once a day: https://github.com/<UPDATE_REPO>/releases/latest/download/update-<chip>.txt, a two-line description
// signed by tools/sign-update.sh (update_parse.h). Installing downloads brewdeck-<version>-<chip>.bin of that release
// into the free app slot (ota.h), checks its size and SHA-256 against the signed description and restarts into it,
// on trial (rolled back if it does not run properly). Checks and downloads run in a short-lived task of their own.
#include <stdint.h>

void update_begin(const char *running_version);   // once, at start (the version built into this firmware)
void update_tick(uint32_t now_ms);                // every UI loop pass: starts a check or a download when due

#ifdef BREWDECK_DEBUG_HTTP
// Debug: take releases from another place for tests: base "http://host:8000/dir" -> base/update-<chip>.txt and
// base/brewdeck-<version>-<chip>.bin (the test key in include/update_test_key.h is accepted too); "" = GitHub again.
void update_set_base(const char *base);
const char *update_base(void);
#endif
