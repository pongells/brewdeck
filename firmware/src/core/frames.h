#pragma once
// Frame times during shots and steam sessions (debug GET /frames, tools/frames.sh): how long bd_ui_tick took per
// loop pass while live data was shown, per session, the last few kept. The UI task adds, the debug server reads.
#include <stddef.h>
#include <stdint.h>

enum { FR_IDLE = 0, FR_SHOT, FR_STEAM };
void   frames_add(uint32_t now_ms, uint32_t frame_ms, int kind);   // every loop pass; kind: what the machine is doing
size_t frames_report(char *buf, size_t n);                         // text, newest session first
