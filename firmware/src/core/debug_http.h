#pragma once
// Debug web server (debug builds only), port 80, no password: GET /screen.bmp (what the panel shows, 800x480 BMP,
// RGB565, 768 KB; tools/screenshot.sh), /rec* (records the panel into PSRAM every N ms, then one download;
// tools/record.sh), /heap, /tasks, /flash, /roam, /touch?x=&y=&ms= (a remote press),
// /bl?pct=&hz= (backlight), plus one route of the UI's (bd_debug_route). Sends nothing to the machine.
#include "brewdeck_core.h"

void debug_http_begin(void);   // call after board_begin() (needs the panel's frame buffer)
// One extra GET path answered by the UI (bd_debug_route): the request is handed to the UI task, which runs it in
// debug_http_service() (from bd_core_loop), and the answer is waited for (up to 30 s).
void debug_http_route(const char *path, bd_debug_fn fn);
void debug_http_service(void);
