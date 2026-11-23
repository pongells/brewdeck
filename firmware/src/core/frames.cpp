// Frame times during shots and steam sessions (see frames.h). A session starts when the machine starts brewing or
// steaming and ends 3 s after it stops (the result / review still drawing counts). Its first 1.5 s are kept apart:
// the screen opening is one full redraw, expected to be slower than the frames after it.
#include "frames.h"
#include <stdio.h>
#include <string.h>

namespace {
const uint32_t END_MS = 3000, OPEN_MS = 1500;
const int SESSIONS = 4, SLOWEST = 5;
struct Session {
  int kind;
  uint32_t start, last;                       // ms since boot: started, last busy frame
  uint32_t frames, sum, max, max_at, open_max;
  uint32_t over40, over80, over150;           // frames after the opening at least that slow
  uint32_t slow_ms[SLOWEST], slow_at[SLOWEST];   // the slowest frames after the opening (ms, ms after the start)
};
Session g_s[SESSIONS];
int g_cur = -1, g_n = 0;                      // open session (-1: none), sessions kept
int g_next = 0;

void slow_add(Session *s, uint32_t ms, uint32_t at) {
  int k = -1;                                 // replace the smallest kept, if this one is slower
  for (int i = 0; i < SLOWEST; i++) if (k < 0 || s->slow_ms[i] < s->slow_ms[k]) k = i;
  if (ms > s->slow_ms[k]) { s->slow_ms[k] = ms; s->slow_at[k] = at; }
}
}  // namespace

void frames_add(uint32_t now, uint32_t ms, int kind) {
  if (g_cur >= 0) {
    Session *s = &g_s[g_cur];
    if (kind != FR_IDLE && kind != s->kind) g_cur = -1;                       // shot -> steam: a new session
    else if (kind == FR_IDLE && now - s->last > END_MS) { g_cur = -1; return; }
  }
  if (g_cur < 0) {
    if (kind == FR_IDLE) return;
    g_cur = g_next;
    g_next = (g_next + 1) % SESSIONS;
    if (g_n < SESSIONS) g_n++;
    memset(&g_s[g_cur], 0, sizeof g_s[g_cur]);
    g_s[g_cur].kind = kind;
    g_s[g_cur].start = now;
  }
  Session *s = &g_s[g_cur];
  if (kind != FR_IDLE) s->last = now;
  const uint32_t at = now - s->start;
  s->frames++;
  s->sum += ms;
  if (at < OPEN_MS) { if (ms > s->open_max) s->open_max = ms; return; }
  if (ms > s->max) { s->max = ms; s->max_at = at; }
  if (ms >= 40) s->over40++;
  if (ms >= 80) s->over80++;
  if (ms >= 150) s->over150++;
  slow_add(s, ms, at);
}

size_t frames_report(char *buf, size_t n) {
  size_t o = 0;
  o += snprintf(buf + o, n - o, "# frame times (bd_ui_tick) per shot / steam session, newest first; the first 1.5 s "
                                "(screen opening) apart\n");
  for (int k = 0; k < g_n && o < n; k++) {
    const Session *s = &g_s[(g_next - 1 - k + 2 * SESSIONS) % SESSIONS];
    const uint32_t dur = (s->last > s->start ? s->last - s->start : 0) + END_MS;
    o += snprintf(buf + o, n - o,
                  "session kind=%s start_s=%u dur_s=%.1f open=%s frames=%u avg_ms=%u max_ms=%u max_at_s=%.1f "
                  "open_max_ms=%u over40=%u over80=%u over150=%u slowest=",
                  s->kind == FR_SHOT ? "shot" : "steam", (unsigned)(s->start / 1000), dur / 1000.0,
                  k == 0 && g_cur >= 0 ? "yes" : "no", (unsigned)s->frames, (unsigned)(s->frames ? s->sum / s->frames : 0),
                  (unsigned)s->max, s->max_at / 1000.0, (unsigned)s->open_max, (unsigned)s->over40,
                  (unsigned)s->over80, (unsigned)s->over150);
    bool first = true;
    for (int i = 0; i < SLOWEST && o < n; i++)
      if (s->slow_ms[i]) {
        o += snprintf(buf + o, n - o, "%s%u@%.1f", first ? "" : ",", (unsigned)s->slow_ms[i], s->slow_at[i] / 1000.0);
        first = false;
      }
    if (o < n) o += snprintf(buf + o, n - o, "\n");
  }
  return o < n ? o : n - 1;
}
