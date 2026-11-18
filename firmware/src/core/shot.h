#pragma once
// Debug web server (debug builds only), port 80, no password: GET /screen.bmp (what the panel shows, 800x480 BMP,
// RGB565, 768 KB; tools/screenshot.sh), /heap, /tasks, /flash, /roam, /touch?x=&y=&ms= (a remote press),
// /bl?pct=&hz= (backlight), plus one route of the UI's (bd_debug_route). Sends nothing to the machine.
void shot_begin(void);   // call after board_begin() (needs the panel's frame buffer)
// One extra GET path answered by the UI (bd_debug_route): the request is handed to the UI task, which runs it in
// shot_service() (from bd_core_loop), and the answer is waited for (up to 30 s).
#include "brewdeck_core.h"
void shot_route(const char *path, bd_debug_fn fn);
void shot_service(void);
