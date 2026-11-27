#pragma once
// Firmware updates: writing a new image into the free app slot, the screen's update page (BD_EV_UPDATE), and keeping
// or rolling back the new image after the restart. The screen restarts into it and keeps it only when it runs
// properly: the UI loop alive and Wi-Fi up for 60 s. If the UI loop stops for 20 s, or Wi-Fi is not up within 3 min,
// it goes back to the previous firmware (the bootloader's app rollback). Refused while a shot, steam, hot water or a
// flush runs.
// Images come from a release (update.cpp: downloaded and checked against a signed file) or, in debug builds, are
// pushed from the computer (build.sh <env> --ota -> POST /ota on the debug web server; password OTA_TOKEN in
// include/ota_token.h, git-ignored, made by build.sh).
#include <stddef.h>
#include <stdint.h>
#include "brewdeck_core.h"

// One image at a time. ota_open takes the image's first bytes (its header: an app image for this chip) and erases the
// free slot; ota_write adds the rest; ota_finish checks the whole image and makes it the next boot (on trial).
// Each returns false with msg set ("refused: ...", "write failed ...") and the page on STOPPED; nothing changes then.
// version: the release being installed ("" = not known), from: where it comes from (an address, "github.com").
bool ota_open(uint32_t len, const uint8_t *first, size_t n, const char *version, const char *from, char *msg, size_t cap);
bool ota_write(const uint8_t *b, size_t n, char *msg, size_t cap);
bool ota_finish(char *msg, size_t cap);
void ota_abort(const char *why);   // stop an open image (STOPPED); why is logged

bool ota_machine_busy(char *why, size_t cap);   // a shot, steam, hot water or a flush running: why = "refused: ..."
bool ota_on_trial(void);                        // the running firmware is new and not kept yet (no update then)
void ota_restart(void);         // after INSTALLED: gives the page and the log a moment, then restarts
void ota_begin(void);           // at boot: keeps or rolls back a new image (see above)
void ota_loop_alive(void);      // every UI loop pass: the trial's "UI loop alive"
const char *ota_slot(void);     // "app0 (kept)", "app1 (on trial)", ... for GET /heap
// The UI loop: true (and ev filled) when the update page should change (BD_EV_UPDATE; progress at most every 250 ms)
bool ota_poll(bd_event *ev);

#ifdef BREWDECK_DEBUG_HTTP
#include "tcp.h"
// Debug push: reads `len` bytes of image from c and installs it. true = installed (restart with ota_restart after
// answering); msg says what happened either way.
bool ota_receive(TcpConn &c, size_t len, const char *token, char *msg, size_t cap);
#endif
