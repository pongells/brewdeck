// Profile preview geometry (see preview.h). Per phase: what it drives and the other quantity's limit, kept apart so a
// UI can draw them differently (Brewdeck: bright 3 px drives, dim 2 px limits):
//   pressure phase: pressure target + flow restriction (if any); flow phase: flow target + pressure restriction
// A target without a start value starts from the previous phase's value (rules at s0 below).
// One vertical scale for both (bar and ml/s): y = (h-3) - v * (h-8)/10, i.e. 0..10 over the box (h = 100: 97 - 9.2 v).
// Time: a phase ends on its first stop condition; water/weight/pressure ones are estimated (rules in
// preview_phase_secs). A ramp longer than the estimated phase is compressed so it still reaches its end value:
// the graph shows the profile's shape, not exact timing.
#include <string.h>
#include "preview.h"
#include "brewdeck_core.h"

static float fmin2(float a, float b) { return a < b ? a : b; }
static float fmax2(float a, float b) { return a > b ? a : b; }
static float clampf(float v, float lo, float hi) { return v < lo ? lo : (v > hi ? hi : v); }

float preview_phase_secs(const PhaseShape *p) {
  const float ramp = p->ramp_ms / 1000.0f;
  float c = 1e9f;
  if (p->stop_ms) c = fmin2(c, p->stop_ms / 1000.0f);
  if (p->stop_water > 0) {
    // expected pump flow: the flow target on a flow phase; on a pressure phase the restriction is only a ceiling
    // (measured 2026-11-04: 1.0 ml/s under a 1.5 limit), so 0.7 x the limit (capped at 2), else a typical 1.5 ml/s
    float f = p->type == PHASE_FLOW ? fmax2(p->end, 0.3f)
                                    : (p->limit > 0 ? 0.7f * fmin2(p->limit, 2.0f) : 1.5f);
    c = fmin2(c, p->stop_water / f);
  }
  if (p->stop_p_above > 0) c = fmin2(c, fmax2(ramp, 3.0f + 0.5f * p->stop_p_above));   // puck fill / pressure build
  if (p->stop_p_below > 0) c = fmin2(c, 3.0f);                                          // pressure decay
  if (p->stop_weight > 0) c = fmin2(c, p->stop_weight / 1.5f);                          // ~1.5 g/s in the cup
  if (c > 1e8f) c = fmax2(ramp, 8.0f);              // no stop condition: runs until the global stop
  return clampf(c, 1.0f, 30.0f);
}

static float ease(uint8_t curve, float u);
// Weight in the cup per phase (rough, like the phase lengths): nothing while the pressure stays low (filling,
// soaking: Leva 9 holds 3 bar with ~0 g in the cup), ~1.5 g/s (at most the flow limit) once a pressure phase
// reaches 4 bar or more (Leva 9 recording: 21 g from ~11 s to 23 s), 0.8 x the flow target on a flow phase that
// is not only a fill under a low pressure limit.
float preview_plan_rate(const PhaseShape *p) {
  if (p->type == PHASE_PRESSURE) return p->end >= 4.0f ? (p->limit > 0 ? fmin2(1.5f, p->limit) : 1.5f) : 0;
  if (p->limit > 0 && p->limit < 3.0f) return 0;   // fill / bloom under a low pressure limit
  return 0.8f * fmin2(p->end, 3.0f);
}

int preview_plan(const ProfileShape *sh, int ph, float spent, int t0_10, int tp0, int tf0, float w0, ShotPoint *out,
                 int cap) {
  if (!sh || ph < 0 || ph >= sh->n || cap < 2) return 0;
  int n = 0;
  float w = w0;
  const float wstop = sh->stop_weight;
  if (wstop > 0 && w0 >= wstop) return 0;            // the shot already reached the stop weight
  float t = 0;                                       // seconds after the stop
  float prev[2] = {tp0 / 10.0f, tf0 / 10.0f};        // last target per quantity (0 pressure, 1 flow)
  auto add = [&](float ts, float p, float f) {
    if (n >= cap) return;
    ShotPoint *s = &out[n++];
    memset(s, 0, sizeof *s);
    const float tt = t0_10 + ts * 10 + 0.5f;
    s->t = (uint16_t)(tt > 65535 ? 65535 : tt);
    s->tp = (uint16_t)(clampf(p, 0, 15) * 10 + 0.5f);
    s->tf = (uint16_t)(clampf(f, 0, 15) * 10 + 0.5f);
    s->ph = (uint8_t)(ph > 255 ? 255 : ph);
  };
  add(0, prev[0], prev[1]);
  for (int i = ph; i < sh->n; i++) {
    const PhaseShape *p = &sh->ph[i];
    const float d = preview_phase_secs(p), from = i == ph ? fmin2(spent, d) : 0;
    if (d - from <= 0.05f) continue;
    const int q = p->type == PHASE_PRESSURE ? 0 : 1;
    const float ramp = p->ramp_ms / 1000.0f, te = fmin2(ramp, d);
    const bool instant = p->curve == CURVE_INSTANT || ramp <= 0 || from >= te;
    // the stopped phase without a start value continues from the target at the stop; others as in the preview
    const float s0 = p->has_start && i != ph ? p->start : prev[q];
    const float u0 = i == ph && !p->has_start ? from : 0;   // ramp progress already behind (current phase)
    const float other = p->limit > 0 ? p->limit : 0;
    int steps = (int)((d - from) / 0.5f);
    if (steps < 1) steps = 1;
    const float rate = preview_plan_rate(p);
    float tend = d;                                  // the stop weight is reached inside this phase: stop there
    const bool last = wstop > 0 && rate > 0 && w + rate * (d - from) >= wstop;
    if (last) {
      tend = from + (wstop - w) / rate;
      steps = (int)((tend - from) / 0.5f);
      if (steps < 1) steps = 1;
    }
    for (int j = 0; j <= steps; j++) {
      const float tp = from + (tend - from) * j / steps;   // time in the phase
      float v;
      if (instant) v = p->end;
      else if (i == ph && p->has_start) v = p->start + (p->end - p->start) * ease(p->curve, clampf(tp / te, 0, 1));
      else v = s0 + (p->end - s0) * ease(p->curve, clampf((tp - u0) / fmax2(te - u0, 0.1f), 0, 1));
      add(t + tp - from, q == 0 ? v : other, q == 1 ? v : other);
    }
    if (last) return n;
    w += rate * (d - from);
    prev[q] = p->end;
    prev[1 - q] = other;
    t += d - from;
  }
  return n;
}

static float ease(uint8_t curve, float u) {
  switch (curve) {
    case CURVE_LINEAR: return u;
    case CURVE_EASE_IN: return u * u;
    case CURVE_EASE_OUT: return 1 - (1 - u) * (1 - u);
    case CURVE_EASE_IN_OUT: return u < 0.5f ? 2 * u * u : 1 - 2 * (1 - u) * (1 - u);
    default: return 1;
  }
}

typedef struct {
  PvSeries *s;
  bool open;          // a segment is in progress
  bool has_prev;      // previous value known (for "start from where the last phase ended")
  float prev;
} Track;

static void put(Track *t, int x, int y) {
  PvSeries *s = t->s;
  if (s->n >= PV_MAX_PTS) return;
  if (!t->open) {
    if (s->nseg >= PV_MAX_SEG) return;
    s->seg[s->nseg++] = s->n;
    t->open = true;
  }
  const int first = s->seg[s->nseg - 1];
  if (s->n > first) {                               // drop exact duplicates
    PvPt *l = &s->pts[s->n - 1];
    if (l->x == x && l->y == y) return;
  }
  if (s->n - first >= 2) {
    // last point within 0.5 px of the straight line from the one before to the new one: move it instead of adding
    // (fewer joints: flat runs become one piece, ramps lose 1 px kinks; every joint is a rounded cap on screen)
    const PvPt a = s->pts[s->n - 2], b = s->pts[s->n - 1];
    const long dx = x - a.x, dy = y - a.y, cr = (long)(b.x - a.x) * dy - (long)(b.y - a.y) * dx;
    const bool between = (b.x - a.x) * dx + (b.y - a.y) * dy >= 0 && (x - b.x) * dx + (y - b.y) * dy >= 0;
    if (between && 4 * cr * cr <= dx * dx + dy * dy) {
      s->pts[s->n - 1].x = (int16_t)x;
      s->pts[s->n - 1].y = (int16_t)y;
      return;
    }
  }
  s->pts[s->n].x = (int16_t)x;
  s->pts[s->n].y = (int16_t)y;
  s->n++;
}
static void gap(Track *t) {
  if (!t->open) return;
  PvSeries *s = t->s;
  if (s->n - s->seg[s->nseg - 1] < 2) { s->n = s->seg[s->nseg - 1]; s->nseg--; }   // a lone point draws nothing
  t->open = false;
}

static int ypx(float v, float ybase, float yk) { return (int)(ybase - clampf(v, 0, 10.5f) * yk + 0.5f); }

float preview_build(const ProfileShape *sh, int w, int h, PvSeries out[PV_SERIES]) {
  memset(out, 0, sizeof(PvSeries) * PV_SERIES);
  if (!sh || sh->n == 0) return 0;
  float dur[MAX_PHASES], total = 0;
  for (int i = 0; i < sh->n; i++) total += dur[i] = preview_phase_secs(&sh->ph[i]);
  const float x0 = 3, xw = (float)(w - 7);
  const float ybase = (float)(h - 3), yk = (float)(h - 8) / 10.0f;
  Track tr[PV_SERIES];
  for (int k = 0; k < PV_SERIES; k++) tr[k] = Track{&out[k], false, false, 0};
  float t0 = 0;
  for (int i = 0; i < sh->n; i++) {
    const PhaseShape *p = &sh->ph[i];
    const float d = dur[i];
    const int xa = (int)(x0 + t0 / total * xw + 0.5f), xb = (int)(x0 + (t0 + d) / total * xw + 0.5f);
    for (int q = 0; q < 2; q++) {                    // q 0 = pressure, 1 = flow
      Track *tt = &tr[q == 0 ? PV_P_TARGET : PV_F_TARGET], *tl = &tr[q == 0 ? PV_P_LIMIT : PV_F_LIMIT];
      if ((q == 0) == (p->type == PHASE_PRESSURE)) {
        // no start value: the previous target, else the previous limit, else 0 for pressure (it builds from
        // rest after a soak) and the end value for flow (unknown: better flat than an invented ramp)
        const float s0 = p->has_start ? p->start : tt->has_prev ? tt->prev : tl->has_prev ? tl->prev
                         : (q == 0 ? 0 : p->end);
        gap(tl);
        tl->has_prev = false;
        const float ramp = p->ramp_ms / 1000.0f;
        const bool instant = p->curve == CURVE_INSTANT || ramp <= 0;
        const float te = fmin2(ramp, d);             // compress ramps longer than the phase
        int steps = instant ? 1 : (xb - xa) / 4;
        if (steps < 1) steps = 1;
        if (tt->open && tt->has_prev) put(tt, xa, ypx(tt->prev, ybase, yk));   // step from the previous end
        for (int j = 0; j <= steps; j++) {
          const float v = instant ? p->end : s0 + (p->end - s0) * ease(p->curve, clampf(d * j / steps / te, 0, 1));
          put(tt, xa + (xb - xa) * j / steps, ypx(v, ybase, yk));
        }
        tt->prev = p->end;
        tt->has_prev = true;
      } else {
        gap(tt);
        tt->has_prev = false;
        if (p->limit > 0) {                    // the other quantity is only limited: draw the limit
          if (tl->open && tl->has_prev) put(tl, xa, ypx(tl->prev, ybase, yk));
          put(tl, xa, ypx(p->limit, ybase, yk));
          put(tl, xb, ypx(p->limit, ybase, yk));
          tl->prev = p->limit;
          tl->has_prev = true;
        } else {
          gap(tl);
          tl->has_prev = false;
        }
      }
    }
    t0 += d;
  }
  for (int k = 0; k < PV_SERIES; k++) {
    gap(&tr[k]);
    if (out[k].nseg) out[k].seg[out[k].nseg] = out[k].n;
  }
  return total;
}

// the core API's preview (brewdeck_core.h): the same geometry, for a box of the UI's size
static_assert(sizeof(bd_preview_series) == sizeof(PvSeries) && sizeof(bd_point) == sizeof(PvPt) &&
              BD_PV_SERIES == PV_SERIES && BD_PV_FLOW == PV_F_TARGET && BD_PV_PRESSURE_LIMIT == PV_P_LIMIT &&
              sizeof(bd_profile_shape) == sizeof(ProfileShape), "preview layout");
bool bd_profile_preview(const bd_profile_shape *s, int w, int h, bd_preview *out) {
  out->secs = preview_build(s, w, h, (PvSeries *)out->s);
  return out->secs > 0;
}
