// Frame times per shot / steam session (firmware/src/core/frames.cpp). Run via tests/run.sh.
#include <stdio.h>
#include <string.h>
#include "frames.h"

static int fail = 0;
static void expect(bool ok, const char *what) { if (!ok) { printf("   FAIL %s\n", what); fail++; } }

int main() {
  static char rep[1600];
  frames_report(rep, sizeof rep);
  expect(!strstr(rep, "session kind"), "no session before anything happened");
  uint32_t t = 1000;
  for (int i = 0; i < 100; i++, t += 30) frames_add(t, 8, FR_IDLE);       // idle: nothing kept
  frames_add(t, 140, FR_SHOT); t += 30;                                   // the brewing screen opening
  for (int i = 0; i < 1000; i++, t += 30) frames_add(t, i == 500 ? 112 : i == 600 ? 45 : 9, FR_SHOT);
  for (int i = 0; i < 50; i++, t += 30) frames_add(t, 20, FR_IDLE);      // the result, 1.5 s: still the shot
  frames_add(t, 7, FR_STEAM); t += 30;                                    // steam right after: a new session
  for (int i = 0; i < 100; i++, t += 30) frames_add(t, 12, FR_STEAM);
  for (int i = 0; i < 200; i++, t += 30) frames_add(t, 5, FR_IDLE);       // 6 s idle: closed
  frames_report(rep, sizeof rep);
  const char *steam = strstr(rep, "kind=steam"), *shot = strstr(rep, "kind=shot");
  expect(steam && shot && steam < shot, "two sessions, newest first");
  expect(shot && strstr(shot, "open_max_ms=140 "), "the opening apart");
  expect(shot && strstr(shot, "max_ms=112 "), "longest frame after the opening");
  expect(shot && strstr(shot, "over40=2 over80=1 over150=0"), "slow frame counts");
  expect(shot && strstr(shot, "frames=1051 "), "frames include the result after the shot");
  expect(steam && strstr(steam, "open=no"), "closed after 3 s idle");
  printf("%s", rep);
  printf("frames_test: %s\n", fail ? "FAILED" : "all passed");
  return fail ? 1 : 0;
}
