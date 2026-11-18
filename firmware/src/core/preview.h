#pragma once
// Profile preview geometry: turns the active profile's phases into polylines (pure C++, no LVGL; host-testable).
// SCHEMATIC: phases mostly end on water/weight/pressure conditions, so their lengths are estimates (see preview.cpp).
#include <stdint.h>
#include "live.h"

#define PV_MAX_PTS 200
#define PV_MAX_SEG MAX_PHASES

typedef struct { int16_t x, y; } PvPt;
typedef struct {
  int  n;                       // points used
  int  nseg;                    // polylines: segment i = pts[seg[i] .. seg[i+1]) (seg[nseg] == n)
  int  seg[PV_MAX_SEG + 1];
  PvPt pts[PV_MAX_PTS];
} PvSeries;

// Four series: the phase's target (solid line) and the other quantity's limit (dashed line), per quantity.
enum { PV_P_TARGET = 0, PV_P_LIMIT, PV_F_TARGET, PV_F_LIMIT, PV_SERIES };
// Fills out[PV_SERIES] for a w x h box. Returns the estimated shot length in seconds (0 = empty).
float preview_build(const ProfileShape *s, int w, int h, PvSeries out[PV_SERIES]);
float preview_phase_secs(const PhaseShape *p);   // estimated phase length (exposed for tests)
// The profile's planned rest after a shot stopped (the shot review's "Target" view): from phase `ph`,
// `spent` s into it, as shot points (t from t0_10 in 0.1 s; tp/tf x10, the rest 0) with the preview's estimated
// phase lengths and ramp rules. tp0/tf0 = the targets at the stop (start of a phase without a start value).
// w0 = weight at the stop (g); the plan ends when the estimated weight reaches the profile's stop weight
// (sh->weight; preview_plan_rate per phase), else at the end of the last phase.
// Returns the number of points (0 = nothing left or no profile).
int preview_plan(const ProfileShape *sh, int ph, float spent, int t0_10, int tp0, int tf0, float w0, ShotPoint *out,
                 int cap);
float preview_plan_rate(const PhaseShape *p);   // estimated weight gain in the cup, g/s (exposed for tests)
