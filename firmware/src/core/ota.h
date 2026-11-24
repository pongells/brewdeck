#pragma once
// Debug builds: firmware updates pushed from the computer (build.sh <env> --ota -> POST /ota on the debug web server).
// The image goes into the free app slot; the screen restarts into it and keeps it only when it runs properly: the UI
// loop alive and Wi-Fi up for 60 s. If the UI loop stops for 20 s, or Wi-Fi is not up within 3 min, it goes back to
// the previous firmware (the bootloader's app rollback). Refused while a shot, steam, hot water or a flush runs.
// Password: OTA_TOKEN in include/ota_token.h (git-ignored, made by build.sh).
#include <stddef.h>
#include "brewdeck_core.h"

#include <WiFi.h>   // WiFiClient (a typedef in this core)
// Reads `len` bytes of image from c and installs it. true = installed (restart with ota_restart after answering);
// msg says what happened either way.
bool ota_receive(WiFiClient &c, size_t len, const char *token, char *msg, size_t cap);
void ota_restart(void);         // after the answer is sent: gives the log a moment to reach the log server
void ota_begin(void);           // at boot: keeps or rolls back a new image (see above)
const char *ota_slot(void);     // "app0 (kept)", "app1 (on trial)", ... for GET /heap
// The UI loop: true (and ev filled) when the update page should change (BD_EV_UPDATE; progress at most every 250 ms)
bool ota_poll(bd_event *ev);
