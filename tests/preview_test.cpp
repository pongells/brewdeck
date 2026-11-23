// Profile preview geometry (firmware/src/core/preview.cpp) on real profiles (sim/data). Run via tests/run.sh.
// args: [-v] profile_*.json ...   (-v also prints an ASCII plot: P/F = pressure/flow target, p/f = limit)
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "parse.h"
#include "preview.h"
#include "test_util.h"
static void plot(const PvSeries *o) {   // 84x84 box shown at 1/4 x, 1/6 y
  char g[15][85]; memset(g, ' ', sizeof g); for (int r = 0; r < 15; r++) g[r][84] = 0;
  const char c[4] = {'P', 'p', 'F', 'f'};   // capital = target, small = limit
  for (int k = 0; k < 4; k++) for (int i = 0; i < o[k].n; i++) {
    int x = o[k].pts[i].x / 4, y = o[k].pts[i].y / 6; if (x >= 0 && x < 84 && y >= 0 && y < 15) g[y][x] = c[k]; }
  for (int r = 0; r < 15; r++) printf("   |%s\n", g[r]);
}
int main(int argc, char **argv) {
  int &fail = fails;   // counted by hand here: each failure also prints why
  const bool verbose = argc > 1 && !strcmp(argv[1], "-v");
  for (int a = verbose ? 2 : 1; a < argc; a++) {
    const char *buf = slurp(argv[a]);
    static bd_profile_shape s; static PvSeries o[PV_SERIES];
    if (!parse_profile_shape(buf, &s)) { printf("%s: parse failed\n", argv[a]); fail++; continue; }
    float tot = preview_build(&s, 336, 84, o);
    printf("%s: %d phases, weight %.1f, temp %.1f, est %.1f s, pts/seg P %d/%d p %d/%d F %d/%d f %d/%d\n   durations:",
           argv[a], s.n, s.stop_weight, s.brew_temp, tot, o[0].n, o[0].nseg, o[1].n, o[1].nseg, o[2].n, o[2].nseg, o[3].n, o[3].nseg);
    { float dur[BD_MAX_PHASES]; preview_phase_lengths(&s, dur, NULL); for (int i = 0; i < s.n; i++) printf(" %.1f", dur[i]); printf("\n"); }
    for (int k = 0; k < 4; k++) { const PvSeries *q = &o[k];
      for (int i = 0; i < q->n; i++) if (q->pts[i].x < 0 || q->pts[i].x > 335 || q->pts[i].y < 0 || q->pts[i].y > 83) { printf("   OUT OF BOX %d,%d\n", q->pts[i].x, q->pts[i].y); fail++; break; }
      if (q->nseg && q->seg[q->nseg] != q->n) { printf("   bad seg end\n"); fail++; } }
    if (verbose) plot(o);
    {   // planned rest: from the start it spans the whole estimate; from the middle it is shorter; time never goes back
      static bd_shot_point pl[400];
      const float wsave = s.stop_weight; s.stop_weight = 0;   // no stop weight: the whole estimate (an open-ended
      static PvSeries o0[PV_SERIES];                         // last phase then gets the default length)
      const float tot0 = preview_build(&s, 336, 84, o0);
      const int n0 = preview_plan(&s, 0, 0, 0, 0, 0, 0, pl, 400);
      if (n0 < 2 || abs((int)pl[n0 - 1].t - (int)(tot0 * 10 + 0.5f)) > 2) { printf("   plan from 0: %d pts, end %d vs %.1f s\n", n0, n0 ? pl[n0 - 1].t : 0, tot0); fail++; }
      for (int i = 1; i < n0; i++) if (pl[i].t < pl[i - 1].t || pl[i].tp > 150 || pl[i].tf > 150) { printf("   plan bad at %d\n", i); fail++; break; }
      const int mid = s.n / 2, n1 = preview_plan(&s, mid, 1.0f, 100, 50, 20, 0, pl, 400);
      if (s.n > 1 && (n1 < 2 || pl[0].t != 100 || pl[0].tp != 50 || pl[n1 - 1].t >= 100 + (int)(tot0 * 10))) { printf("   plan from phase %d: %d pts\n", mid, n1); fail++; }
      if (preview_plan(&s, s.n, 0, 0, 0, 0, 0, pl, 400) != 0) { printf("   plan past the end not empty\n"); fail++; }
      s.stop_weight = wsave;   // with the stop weight: never longer, and already reached = ends at once
      const int nw = preview_plan(&s, 0, 0, 0, 0, 0, 0, pl, 400);
      const float wend = nw ? pl[nw - 1].t / 10.0f : 0;
      if (nw < 2 || wend > tot + 0.2f) { printf("   plan with stop weight: %d pts, %.1f s\n", nw, wend); fail++; }
      if (s.stop_weight > 0 && preview_plan(&s, 0, 0, 0, 0, 0, s.stop_weight + 1, pl, 400) > 2) { printf("   plan past the stop weight not short\n"); fail++; }
      printf("   plan: %d points from 0 (%.1f s; to %.0f g %.1f s), %d from phase %d\n", n0, tot, s.stop_weight, wend, n1, mid);
    }
  }
  // truncated reply must fail cleanly
  bd_profile_shape t; const char *trunc = "{\"name\":\"x\",\"phases\":[{\"type\":\"FLOW\",\"target\":{\"end\":3";
  if (parse_profile_shape(trunc, &t)) { printf("truncated accepted!\n"); fail++; }
  static PvSeries o2[PV_SERIES]; bd_profile_shape e; memset(&e, 0, sizeof e);
  if (preview_build(&e, 336, 84, o2) != 0 || o2[0].n || o2[2].n) { printf("empty not empty\n"); fail++; }
  return report("preview_test");
}
