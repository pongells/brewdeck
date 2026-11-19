// Tiny allocation-free JSON readers (no Arduino headers; see parse.h).
// Not a general JSON parser: looks up "key": in a known, flat-enough reply.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include "parse.h"

// copies the value of "key": (string without quotes, or a bare number/bool) into out; searches js[0..end)
static bool raw_n(const char *js, const char *end, const char *key, char *out, size_t n) {
  char pat[48];
  snprintf(pat, sizeof pat, "\"%s\":", key);
  const char *p = strstr(js, pat);
  if (!p || (end && p >= end)) return false;
  p += strlen(pat);
  while (*p == ' ') p++;
  size_t i = 0;
  if (*p == '"') {
    p++;
    while (*p && *p != '"' && i + 1 < n) {
      if (*p == '\\' && p[1] == 'u') {             // \uXXXX (non-ASCII): dropped, like the WebSocket side does
        p += 2;                                     // (pb_profile_info keeps printable ASCII only)
        for (int k = 0; k < 4 && *p && *p != '"'; k++) p++;
        continue;
      }
      if (*p == '\\' && p[1]) p++;
      out[i++] = *p++;
    }
  } else {
    while (*p && *p != ',' && *p != '}' && *p != ']' && *p != ' ' && i + 1 < n) out[i++] = *p++;
  }
  out[i] = 0;
  return true;
}

static float num(const char *js, const char *end, const char *key, float def) {
  char b[24];
  return raw_n(js, end, key, b, sizeof b) ? (float)atof(b) : def;
}
static uint32_t unum(const char *js, const char *end, const char *key) {   // whole numbers above 2^24 (float loses them)
  char b[24];
  return raw_n(js, end, key, b, sizeof b) ? (uint32_t)strtoul(b, NULL, 10) : 0;
}
static bool flag(const char *js, const char *end, const char *key) {
  char b[8];
  return raw_n(js, end, key, b, sizeof b) && b[0] == 't';
}
// keep plain ASCII only (the screen fonts have no accented glyphs)
static void text(const char *js, const char *end, const char *key, char *out, size_t n) {
  char b[64];
  size_t j = 0;
  if (raw_n(js, end, key, b, sizeof b))
    for (size_t i = 0; b[i] && j + 1 < n; i++)
      if ((uint8_t)b[i] >= 32 && (uint8_t)b[i] < 127) out[j++] = b[i];
  out[j] = 0;
}

// returns the position just past the value that starts at p ('{' or '['), skipping strings; NULL if truncated
static const char *skip_value(const char *p) {
  int depth = 0;
  bool str = false;
  for (; *p; p++) {
    if (str) {
      if (*p == '\\' && p[1]) p++;
      else if (*p == '"') str = false;
      continue;
    }
    if (*p == '"') str = true;
    else if (*p == '{' || *p == '[') depth++;
    else if (*p == '}' || *p == ']') { if (--depth == 0) return p + 1; }
  }
  return NULL;
}

void parse_status(const char *js, LiveData *d) {
  memset(d, 0, sizeof *d);
  d->temperature = num(js, NULL, "temperature", 0);
  d->target = num(js, NULL, "targetTemperature", 0);
  d->pressure = num(js, NULL, "pressure", 0);
  d->weight = num(js, NULL, "weight", 0);
  d->water_level = (int)num(js, NULL, "waterLevel", 0);
  d->warmup_remaining = (int)num(js, NULL, "warmupRemainingSeconds", 0);
  d->readiness = (int)num(js, NULL, "readinessState", -1);
  d->brew_active = flag(js, NULL, "brewActive");
  d->steam_active = flag(js, NULL, "steamActive");
  d->hot_water_active = flag(js, NULL, "hotWaterActive");
  d->scales_present = flag(js, NULL, "scalesPresent");
  text(js, NULL, "profileName", d->profile, sizeof d->profile);
  text(js, NULL, "machineName", d->machine, sizeof d->machine);
  text(js, NULL, "operationMode", d->mode, sizeof d->mode);
  text(js, NULL, "coreVersion", d->core_ver, sizeof d->core_ver);
  text(js, NULL, "coreType", d->core_type, sizeof d->core_type);
}

// [{"id":1,"name":"IUIUIU Classic","selected":"false"}, ...]
int parse_profile_list(const char *js, ProfileList *out) {
  memset(out, 0, sizeof *out);
  const char *p = strchr(js, '[');
  if (!p) return 0;
  p++;
  while (out->count < MAX_PROFILES) {
    p = strchr(p, '{');
    if (!p) break;
    const char *e = skip_value(p);
    if (!e) break;                                     // truncated reply: keep what is complete
    ProfileInfo *pi = &out->p[out->count];
    char b[16];
    if (raw_n(p, e, "id", b, sizeof b)) {
      pi->id = atoi(b);
      text(p, e, "name", pi->name, sizeof pi->name);
      pi->selected = flag(p, e, "selected");
      out->count++;
    }
    p = e;
  }
  return out->count;
}

// bounds [*b, *e) of the object value of "key": inside [p, end); false if absent
static bool sub_object(const char *p, const char *end, const char *key, const char **b, const char **e) {
  char pat[40];
  snprintf(pat, sizeof pat, "\"%s\":", key);
  const char *k = strstr(p, pat);
  if (!k || k >= end) return false;
  const char *o = strchr(k, '{');
  if (!o || o >= end) return false;
  const char *c = skip_value(o);
  if (!c || c > end) return false;
  *b = o;
  *e = c;
  return true;
}

static uint8_t curve_of(const char *s) {
  if (!strcmp(s, "LINEAR")) return CURVE_LINEAR;
  if (!strcmp(s, "EASE_IN")) return CURVE_EASE_IN;
  if (!strcmp(s, "EASE_OUT")) return CURVE_EASE_OUT;
  if (!strcmp(s, "EASE_IN_OUT")) return CURVE_EASE_IN_OUT;
  return CURVE_INSTANT;
}

// phases of one profile:  {"type":"PRESSURE","target":{"start":3,"end":9,"curve":"EASE_IN_OUT","time":5000},
//   "restriction":2.5,"stopConditions":{"time":7000,"pressureAbove":3,"weight":10,"waterPumpedInPhase":3.5},"skip":false}
bool parse_profile_shape(const char *js, ProfileShape *s) {
  memset(s, 0, sizeof *s);
  const char *ph = strstr(js, "\"phases\":");
  if (!ph) return false;
  ph = strchr(ph, '[');
  if (!ph) return false;
  const char *ph_end = skip_value(ph);
  if (!ph_end) return false;                           // truncated
  for (const char *q = ph + 1; q < ph_end - 1 && s->n < MAX_PHASES;) {
    while (q < ph_end - 1 && *q != '{') q++;
    if (q >= ph_end - 1) break;
    const char *e = skip_value(q);
    if (!e) return false;
    if (!flag(q, e, "skip")) {
      PhaseShape *p = &s->ph[s->n];
      char b[16];
      p->type = (raw_n(q, e, "type", b, sizeof b) && !strcmp(b, "FLOW")) ? PHASE_FLOW : PHASE_PRESSURE;
      const char *tb, *te;
      if (sub_object(q, e, "target", &tb, &te)) {
        p->has_start = raw_n(tb, te, "start", b, sizeof b);
        p->start = p->has_start ? (float)atof(b) : 0;
        p->end = num(tb, te, "end", 0);
        p->ramp_ms = (uint32_t)num(tb, te, "time", 0);
        p->curve = raw_n(tb, te, "curve", b, sizeof b) ? curve_of(b) : CURVE_INSTANT;
      }
      p->limit = num(q, e, "restriction", 0);   // phase-level key (not inside target / stopConditions)
      if (sub_object(q, e, "stopConditions", &tb, &te)) {
        p->stop_ms = (uint32_t)num(tb, te, "time", 0);
        p->stop_p_above = num(tb, te, "pressureAbove", 0);
        p->stop_p_below = num(tb, te, "pressureBelow", 0);
        p->stop_water = num(tb, te, "waterPumpedInPhase", 0);
        p->stop_weight = num(tb, te, "weight", 0);
      }
      s->n++;
    }
    q = e;
  }
  ProfileInfo tmp;
  memset(&tmp, 0, sizeof tmp);
  if (parse_profile_detail(js, &tmp)) {
    s->stop_weight = tmp.weight;
    s->brew_temp = tmp.temp;
  }
  return true;
}

// {"name":"...","phases":[{...},{...}],"globalStopConditions":{"weight":21},"waterTemperature":90,...}
bool parse_profile_detail(const char *js, ProfileInfo *pi) {
  const char *ph = strstr(js, "\"phases\":");
  if (!ph) return false;
  ph = strchr(ph, '[');
  if (!ph) return false;
  const char *ph_end = skip_value(ph);
  if (!ph_end) return false;                           // truncated
  int n = 0;
  for (const char *q = ph + 1; q < ph_end - 1;) {      // count the top-level objects in the phases array
    while (q < ph_end - 1 && *q != '{') q++;
    if (q >= ph_end - 1) break;
    const char *e = skip_value(q);
    if (!e) break;
    n++;
    q = e;
  }
  pi->phases = n;
  // stop weight and temperature live OUTSIDE the phases array (phases have their own stop conditions)
  pi->weight = 0;
  const char *g = strstr(ph_end, "\"globalStopConditions\":");
  if (!g) { g = strstr(js, "\"globalStopConditions\":"); if (g && g > ph && g < ph_end) g = NULL; }
  if (g) {
    const char *gb = strchr(g, '{');
    const char *ge = gb ? skip_value(gb) : NULL;
    if (gb && ge) pi->weight = num(gb, ge, "weight", 0);
  }
  const char *t = strstr(ph_end, "\"waterTemperature\":");
  if (!t) { t = strstr(js, "\"waterTemperature\":"); if (t && t > ph && t < ph_end) t = NULL; }
  pi->temp = t ? num(t, NULL, "waterTemperature", 0) : 0;
  pi->detail = true;
  return true;
}

// ---- stored shots ------------------------------------------------------------------------------------------------
// GET /api/shots/latest -> [{"lastShotId":298}]
int parse_shots_latest(const char *js) {
  return (int)num(js, NULL, "lastShotId", 0);
}

// "key":[1,2,3] inside [b, e): returns the position after '[' or NULL
static const char *int_array(const char *b, const char *e, const char *key) {
  char pat[40];
  snprintf(pat, sizeof pat, "\"%s\":[", key);
  const char *p = strstr(b, pat);
  return p && p < e ? p + strlen(pat) : NULL;
}

// GET /api/shots/<id> (real reply in sim/data/shot_298.json):
// {"id":298,"timestamp":...,"duration":73,"datapoints":{"timeInShot":[4,5,...],"pressure":[...],"pumpFlow":[...],
//  "weightFlow":[...],"temperature":[...],"shotWeight":[...],"waterPumped":[...],"targetTemperature":[...],
//  "targetPumpFlow":[...],"targetPressure":[...],"phaseIndex":[...]},"profile":{"id":8,"name":"...",...}}
// All arrays are x10 integers of the same length. Points beyond SHOT_MAX are thinned (every k-th kept).
bool parse_shot(const char *js, ShotTrace *t) {
  memset(t, 0, sizeof *t);
  const char *jend = js + strlen(js), *db, *de;
  if (!sub_object(js, jend, "datapoints", &db, &de)) return false;
  static const char *keys[] = {"timeInShot", "pressure", "pumpFlow", "shotWeight", "temperature", "targetPressure",
                               "targetPumpFlow", "phaseIndex"};
  // length from the time array
  const char *p = int_array(db, de, keys[0]);
  if (!p) return false;
  int n = 0;
  const char *q = p;
  while (*q == ' ') q++;
  if (*q != ']') for (n = 1; q < de && *q != ']'; q++) if (*q == ',') n++;
  if (n == 0) return false;
  const int step = (n + SHOT_MAX - 1) / SHOT_MAX;
  int kept = (n + step - 1) / step;
  for (int k = 0; k < 8; k++) {
    p = int_array(db, de, keys[k]);
    if (!p) { if (k == 7) continue; return false; }   // phaseIndex may be missing: stays 0
    for (int i = 0; i < n; i++) {
      char *x;
      const long v = strtol(p, &x, 10);
      if (x == p || x >= de) return false;            // truncated or not a number
      if (i % step == 0) {
        ShotPoint *s = &t->pt[i / step];
        const uint16_t u = (uint16_t)(v < 0 ? 0 : (v > 65535 ? 65535 : v));
        switch (k) {
          case 0: s->t = u; break;      case 1: s->p = u; break;    case 2: s->f = u; break;
          case 3: s->w = u; break;      case 4: s->temp = u; break; case 5: s->tp = u; break;
          case 6: s->tf = u; break;     default: s->ph = (uint8_t)(u > 255 ? 255 : u); break;
        }
      }
      p = x;
      while (*p == ' ') p++;
      if (i + 1 < n) { if (*p != ',') return false; p++; }
      else if (*p != ']') return false;               // arrays must all have the same length
    }
  }
  t->n = kept;
  t->id = (int)num(js, db, "id", 0);
  t->dur10 = (int)num(js, db, "duration", 0);
  t->ts = unum(js, db, "timestamp");
  const char *pb, *pe;
  if (sub_object(de, jend, "profile", &pb, &pe)) text(pb, pe, "name", t->profile, sizeof t->profile);
  return true;
}
