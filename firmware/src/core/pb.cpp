// See pb.h. Wire types: 0 varint, 1 fixed64, 2 length-delimited, 5 fixed32.
#include <string.h>
#include "pb.h"

typedef struct { const uint8_t *p, *end; bool ok; } Rd;

static uint64_t varint(Rd *r) {
  uint64_t v = 0;
  for (int s = 0; s < 64; s += 7) {
    if (r->p >= r->end) { r->ok = false; return 0; }
    const uint8_t c = *r->p++;
    v |= (uint64_t)(c & 0x7F) << s;
    if (c < 0x80) return v;
  }
  r->ok = false;
  return 0;
}

static float f32(Rd *r) {
  if (r->end - r->p < 4) { r->ok = false; r->p = r->end; return 0; }
  uint32_t u = (uint32_t)r->p[0] | (uint32_t)r->p[1] << 8 | (uint32_t)r->p[2] << 16 | (uint32_t)r->p[3] << 24;
  r->p += 4;
  float f;
  memcpy(&f, &u, 4);
  return f;
}

// reads one field header; false at the end or on a malformed frame
static bool next(Rd *r, uint32_t *no, uint32_t *wt) {
  if (!r->ok || r->p >= r->end) return false;
  const uint64_t k = varint(r);
  *no = (uint32_t)(k >> 3);
  *wt = (uint32_t)(k & 7);
  return r->ok;
}

static void skip(Rd *r, uint32_t wt) {
  size_t n = 0;
  switch (wt) {
    case 0: varint(r); return;
    case 1: n = 8; break;
    case 2: n = (size_t)varint(r); break;
    case 5: n = 4; break;
    default: r->ok = false; return;
  }
  if (!r->ok || (size_t)(r->end - r->p) < n) { r->ok = false; r->p = r->end; return; }
  r->p += n;
}

// printable ASCII only: the screen fonts have nothing else, and names are compared with the REST ones
static void copy_ascii(const uint8_t *p, size_t l, char *o, size_t on) {
  size_t k = 0;
  for (size_t j = 0; j < l && k + 1 < on; j++)
    if (p[j] >= 32 && p[j] < 127) o[k++] = (char)p[j];
  o[k] = 0;
}

bool pb_envelope(const uint8_t *b, size_t n, char *act, size_t cap, const uint8_t **data, size_t *dlen) {
  Rd r = {b, b + n, true};
  uint32_t no, wt;
  act[0] = 0;
  *data = NULL;
  *dlen = 0;
  while (next(&r, &no, &wt)) {
    if ((no == 1 || no == 2) && wt == 2) {
      const size_t len = (size_t)varint(&r);
      if (!r.ok || (size_t)(r.end - r.p) < len) return false;
      if (no == 1) {
        const size_t k = len < cap - 1 ? len : cap - 1;
        memcpy(act, r.p, k);
        act[k] = 0;
      } else {
        *data = r.p;
        *dlen = len;
      }
      r.p += len;
    } else {
      skip(&r, wt);
    }
  }
  return r.ok && act[0];
}

bool pb_sensor(const uint8_t *b, size_t n, WsSensor *o) {
  memset(o, 0, sizeof *o);
  Rd r = {b, b + n, true};
  uint32_t no, wt;
  while (next(&r, &no, &wt)) {
    if (wt == 0 && no <= 3) {
      const bool v = varint(&r) != 0;
      if (no == 1) o->brew_active = v;
      else if (no == 2) o->steam_active = v;
      else o->hot_water = v;
    } else if (wt == 5 && no >= 4 && no <= 9) {
      const float f = f32(&r);
      switch (no) {
        case 4: o->temperature = f; break;
        case 6: o->pressure = f; break;
        case 7: o->pump_flow = f; break;
        case 8: o->weight_flow = f; break;
        case 9: o->weight = f; break;
        default: break;                    // 5 waterTemperature (same as temperature on this machine)
      }
    } else if (wt == 0 && no == 10) {
      o->water_level = (int)varint(&r);
    } else {
      skip(&r, wt);
    }
  }
  return r.ok;
}

// NotificationDto { NotificationTypeDto type = 1; string message = 2; }
bool pb_notif(const uint8_t *b, size_t n, WsNotif *o) {
  memset(o, 0, sizeof *o);
  Rd r = {b, b + n, true};
  uint32_t no, wt;
  while (next(&r, &no, &wt)) {
    if (no == 1 && wt == 0) o->type = (int)varint(&r);
    else if (no == 2 && wt == 2) {
      const size_t l = (size_t)varint(&r);
      if (!r.ok || (size_t)(r.end - r.p) < l) return false;
      copy_ascii(r.p, l, o->msg, sizeof o->msg);
      r.p += l;
    } else skip(&r, wt);
  }
  if (o->type < NOTIF_INFO || o->type > NOTIF_ERROR) o->type = NOTIF_INFO;
  return r.ok && o->msg[0];
}

bool pb_system(const uint8_t *b, size_t n, WsSystem *o) {
  memset(o, 0, sizeof *o);                 // mode 0 = BREW_AUTO: an absent field means BREW_AUTO
  Rd r = {b, b + n, true};
  uint32_t no, wt;
  while (next(&r, &no, &wt)) {
    if (wt != 0) { skip(&r, wt); continue; }
    const uint64_t v = varint(&r);
    switch (no) {
      case 4: o->scales_present = v != 0; break;
      case 5: o->mode = (int)v; break;
      case 8: o->tare_pending = v != 0; break;
      case 30: o->persisted_profile = (uint32_t)v; break;
      case 33: o->readiness = (uint32_t)v; break;
      case 35: o->warmup_remaining = (uint32_t)v; break;
      default: break;
    }
  }
  return r.ok;
}

bool pb_shot(const uint8_t *b, size_t n, WsShot *o) {
  memset(o, 0, sizeof *o);
  Rd r = {b, b + n, true};
  uint32_t no, wt;
  uint32_t *f[12] = {NULL, &o->time10, &o->pressure10, &o->pump_flow10, &o->weight_flow10, &o->temperature10,
                     &o->weight10, &o->water10, &o->target_temp10, &o->target_flow10, &o->target_pressure10,
                     &o->phase};
  while (next(&r, &no, &wt)) {
    if (wt == 0 && no >= 1 && no <= 11) *f[no] = (uint32_t)varint(&r);
    else skip(&r, wt);
  }
  return r.ok;
}

const char *ws_mode_name(int m) {
  static const char *const N[] = {"BREW_AUTO", "BREW_MANUAL", "FLUSH", "DESCALE", "STEAM", "FLUSH_AUTO",
                                  "HOT_WATER", "HOME", "PUMP_CALIBRATION", "POWER_OFF"};
  return m >= 0 && m < (int)(sizeof N / sizeof N[0]) ? N[m] : "";
}

uint32_t shot_name_hash(const char *s, size_t n) {
  uint32_t h = 2166136261u;
  for (size_t i = 0; i < n && s[i]; i++) {
    const uint8_t c = (uint8_t)s[i];
    if (c < 32 || c > 126) continue;   // ASCII only, like the names the REST readers keep
    h ^= c;
    h *= 16777619u;
  }
  return h;
}

// ShotHistoryIndexDto { repeated ShotHistoryIndexRecordDto records = 1; }
// ShotHistoryIndexRecordDto { uint32 id = 1; string profileName = 2; uint32 timestamp = 3; uint32 duration = 4; }
bool pb_shot_index(const uint8_t *b, size_t n, WsShotIndex *o) {
  memset(o, 0, sizeof *o);
  Rd r = {b, b + n, true};
  uint32_t no, wt;
  while (next(&r, &no, &wt)) {
    if (no != 1 || wt != 2) { skip(&r, wt); continue; }
    const size_t len = (size_t)varint(&r);
    if (!r.ok || (size_t)(r.end - r.p) < len) return false;
    Rd s = {r.p, r.p + len, true};
    r.p += len;
    WsShotRec rec;
    memset(&rec, 0, sizeof rec);
    rec.name_hash = shot_name_hash("", 0);
    while (next(&s, &no, &wt)) {
      if (no == 1 && wt == 0) rec.id = (uint32_t)varint(&s);
      else if (no == 3 && wt == 0) rec.ts = (uint32_t)varint(&s);
      else if (no == 4 && wt == 0) rec.dur10 = (uint32_t)varint(&s);
      else if (no == 2 && wt == 2) {
        const size_t l = (size_t)varint(&s);
        if (!s.ok || (size_t)(s.end - s.p) < l) return false;
        rec.name_hash = shot_name_hash((const char *)s.p, l);
        copy_ascii(s.p, l, rec.name, sizeof rec.name);
        s.p += l;
      } else skip(&s, wt);
    }
    if (!s.ok) return false;
    if (o->n < WS_SHOT_INDEX_MAX) o->r[o->n++] = rec;
  }
  return r.ok;
}

const uint8_t PB_TARE_CMD[] = {0x0A, 11, 'c', '_', 't', 'a', 'r', 'e', '_', 'p', 'e', 'n', 'd',   // 1: action
                               0x12, 2, 0x10, 0x01};                                             // 2: data {2: true}
const size_t PB_TARE_CMD_LEN = sizeof PB_TARE_CMD;

const uint8_t PB_SAVE_PROF_CMD[] = {0x0A, 15, 'c', '_', 's', 'a', 'v', 'e', '_', 'a', 'c', 't', '_', 'p', 'r', 'o', 'f'};
const size_t PB_SAVE_PROF_CMD_LEN = sizeof PB_SAVE_PROF_CMD;

size_t pb_opmode_cmd(int mode, uint8_t *out, size_t cap) {
  if (mode != PB_MODE_FLUSH && mode != PB_MODE_FLUSH_AUTO && mode != PB_MODE_HOME) return 0;   // flushing only
  static const char A[] = "c_opmode";
  const size_t n = 2 + 8 + 2 + 2;
  if (!out || cap < n) return 0;
  size_t k = 0;
  out[k++] = 0x0A; out[k++] = 8;                     // 1: action
  memcpy(out + k, A, 8); k += 8;
  out[k++] = 0x12; out[k++] = 2;                     // 2: data
  out[k++] = 0x08; out[k++] = (uint8_t)mode;         //    { 1: operationMode }
  return k;
}

bool pb_scale_cfg(const uint8_t *b, size_t n, WsScaleCfg *o) {
  memset(o, 0, sizeof *o);
  Rd r = {b, b + n, true};
  uint32_t no, wt;
  while (next(&r, &no, &wt)) {
    if (no != 4 || wt != 2) { skip(&r, wt); continue; }   // boiler, system, led, display, theme, versions, webUi
    const size_t len = (size_t)varint(&r);
    if (!r.ok || (size_t)(r.end - r.p) < len) return false;
    Rd s = {r.p, r.p + len, true};
    r.p += len;
    uint32_t sn, sw;
    while (next(&s, &sn, &sw)) {
      if (sw != 0) { skip(&s, sw); continue; }          // hwScalesF1/F2 are varints too, but ignored; pinned MAC skipped
      const bool v = varint(&s) != 0;
      if (sn == 1) o->force_predictive = v;
      else if (sn == 2) o->hw_enabled = v;
      else if (sn == 5) o->bt_enabled = v;
    }
    if (!s.ok) return false;
    o->valid = true;
  }
  return r.ok && o->valid;
}

bool pb_versions(const uint8_t *b, size_t n, char *front, size_t fn, char *stat, size_t sn) {
  front[0] = stat[0] = 0;
  bool found = false;
  Rd r = {b, b + n, true};
  uint32_t no, wt;
  while (next(&r, &no, &wt)) {
    if (no != 7 || wt != 2) { skip(&r, wt); continue; }   // only GaggiaSettingsDto.versions
    const size_t len = (size_t)varint(&r);
    if (!r.ok || (size_t)(r.end - r.p) < len) return false;
    Rd s = {r.p, r.p + len, true};
    r.p += len;
    found = true;
    uint32_t sn2, sw;
    while (next(&s, &sn2, &sw)) {
      if ((sn2 == 2 || sn2 == 3) && sw == 2) {           // frontVersion / staticVersion; coreVersion (1) skipped
        const size_t l = (size_t)varint(&s);
        if (!s.ok || (size_t)(s.end - s.p) < l) return false;
        if (sn2 == 2) copy_ascii(s.p, l, front, fn);
        else copy_ascii(s.p, l, stat, sn);
        s.p += l;
      } else skip(&s, sw);
    }
    if (!s.ok) return false;
  }
  return r.ok && found;
}

bool pb_steam_set(const uint8_t *b, size_t n, uint32_t *deg) {
  *deg = 0;
  bool found = false;
  Rd r = {b, b + n, true};
  uint32_t no, wt;
  while (next(&r, &no, &wt)) {
    if (no != 1 || wt != 2) { skip(&r, wt); continue; }   // only GaggiaSettingsDto.boiler
    const size_t len = (size_t)varint(&r);
    if (!r.ok || (size_t)(r.end - r.p) < len) return false;
    Rd s = {r.p, r.p + len, true};
    r.p += len;
    uint32_t sn, sw;
    while (next(&s, &sn, &sw)) {
      if (sn == 1 && sw == 0) { *deg = (uint32_t)varint(&s); found = true; }   // steamSetPoint; the rest skipped
      else skip(&s, sw);
    }
    if (!s.ok) return false;
  }
  return r.ok && found;
}

bool pb_profile_info(const uint8_t *b, size_t n, uint32_t *id, char *name, size_t ncap, float *weight) {
  *id = 0;
  *weight = 0;
  if (ncap) name[0] = 0;
  Rd r = {b, b + n, true};
  uint32_t no, wt;
  while (next(&r, &no, &wt)) {
    if (no == 6 && wt == 0) { *id = (uint32_t)varint(&r); continue; }
    if ((no == 1 || no == 3) && wt == 2) {
      const size_t len = (size_t)varint(&r);
      if (!r.ok || (size_t)(r.end - r.p) < len) return false;
      if (no == 1 && ncap) {                        // as parse.cpp text(): the REST names are compared with these
        copy_ascii(r.p, len, name, ncap);
      } else if (no == 3) {
        Rd g = {r.p, r.p + len, true};
        uint32_t gn, gw;
        while (next(&g, &gn, &gw)) {
          if (gn == 2 && gw == 5) *weight = f32(&g);
          else skip(&g, gw);
        }
        if (!g.ok) return false;
      }
      r.p += len;
      continue;
    }
    skip(&r, wt);
  }
  return r.ok;
}

typedef struct { uint8_t *p, *end; bool ok; } Wr;
static void w_byte(Wr *w, uint8_t c) { if (w->p < w->end) *w->p++ = c; else w->ok = false; }
static void w_varint(Wr *w, uint64_t v) { do { w_byte(w, (uint8_t)((v & 0x7F) | (v > 0x7F ? 0x80 : 0))); v >>= 7; } while (v); }
static void w_bytes(Wr *w, const uint8_t *b, size_t n) {
  if ((size_t)(w->end - w->p) < n) { w->ok = false; return; }
  memcpy(w->p, b, n);
  w->p += n;
}
static size_t varint_len(uint64_t v) { size_t k = 1; while (v > 0x7F) { v >>= 7; k++; } return k; }
static void w_f32(Wr *w, float f) {
  uint32_t u;
  memcpy(&u, &f, 4);
  for (int i = 0; i < 4; i++) w_byte(w, (uint8_t)(u >> (8 * i)));
}

// GlobalStopConditionsDto with field 2 replaced: copy every other field, then weight
static void w_weight(Wr *w, float weight) {
  uint32_t u;
  memcpy(&u, &weight, 4);
  w_byte(w, 0x15);                                  // field 2, fixed32
  for (int i = 0; i < 4; i++) w_byte(w, (uint8_t)(u >> (8 * i)));
}
static bool gsc_patched(const uint8_t *b, size_t n, float weight, Wr *w) {
  Rd r = {b, b + n, true};
  uint32_t no, wt;
  bool done = false;                                // weight written in field order (as protobuf-ts encodes)
  while (next(&r, &no, &wt)) {
    const uint8_t *f0 = r.p;                        // just after the key
    skip(&r, wt);
    if (!r.ok) return false;
    if (no == 2) continue;                          // the old weight
    if (no > 2 && !done) { w_weight(w, weight); done = true; }
    w_varint(w, (uint64_t)no << 3 | wt);
    w_bytes(w, f0, (size_t)(r.p - f0));
  }
  if (!done) w_weight(w, weight);
  return r.ok && w->ok;
}

// envelope {1: "c_upd_act_prof", 2: data} in front of data already written at out + hdr_max; moves it all to out
static size_t act_wrap(uint8_t *out, size_t hdr_max, size_t dlen) {
  static const char ACT[] = "c_upd_act_prof";
  const size_t hdr = 2 + (sizeof ACT - 1) + 1 + varint_len(dlen);
  uint8_t *h = out + hdr_max - hdr;
  Wr hw = {h, out + hdr_max, true};
  w_byte(&hw, 0x0A);
  w_byte(&hw, (uint8_t)(sizeof ACT - 1));
  w_bytes(&hw, (const uint8_t *)ACT, sizeof ACT - 1);
  w_byte(&hw, 0x12);
  w_varint(&hw, dlen);
  if (!hw.ok || hw.p != out + hdr_max) return 0;
  memmove(out, h, hdr + dlen);
  return hdr + dlen;
}

size_t pb_weight_cmd(const uint8_t *prof, size_t n, float weight, uint8_t *out, size_t cap) {
  if (!(weight >= 0 && weight <= BD_STOP_WEIGHT_MAX)) return 0;   // the API's range (the web UI allows 300)
  // 1) patched profile into the tail of out, 2) envelope header in front of it
  static const char ACT[] = "c_upd_act_prof";
  const size_t hdr_max = 2 + (sizeof ACT - 1) + 1 + 5;   // action field + data key + length varint
  if (cap < hdr_max + 16) return 0;
  Wr w = {out + hdr_max, out + cap, true};
  Rd r = {prof, prof + n, true};
  uint32_t no, wt;
  bool have_gsc = false;
  while (next(&r, &no, &wt)) {
    const uint8_t *f0 = r.p;
    if (no == 3 && wt == 2) {
      const size_t len = (size_t)varint(&r);
      if (!r.ok || (size_t)(r.end - r.p) < len) return 0;
      uint8_t tmp[64];                                // GlobalStopConditionsDto is 5 small fields
      Wr g = {tmp, tmp + sizeof tmp, true};
      if (!gsc_patched(r.p, len, weight, &g)) return 0;
      r.p += len;
      w_byte(&w, 0x1A);                               // field 3, length-delimited
      w_varint(&w, (uint64_t)(g.p - tmp));
      w_bytes(&w, tmp, (size_t)(g.p - tmp));
      have_gsc = true;
      continue;
    }
    skip(&r, wt);
    if (!r.ok) return 0;
    w_varint(&w, (uint64_t)no << 3 | wt);
    w_bytes(&w, f0, (size_t)(r.p - f0));
  }
  if (!r.ok) return 0;
  if (!have_gsc) {                                    // profile without stop conditions: add them
    uint8_t tmp[8];
    Wr g = {tmp, tmp + sizeof tmp, true};
    if (!gsc_patched(NULL, 0, weight, &g)) return 0;
    w_byte(&w, 0x1A);
    w_varint(&w, (uint64_t)(g.p - tmp));
    w_bytes(&w, tmp, (size_t)(g.p - tmp));
  }
  if (!w.ok) return 0;
  return act_wrap(out, hdr_max, (size_t)(w.p - (out + hdr_max)));
}

size_t pb_temp_cmd(const uint8_t *prof, size_t n, float temp, uint8_t *out, size_t cap) {
  // low values are real profiles (a boiler-off test profile at 1 degC); 0 = heater off: never sent
  if (!(temp >= BD_BREW_TEMP_MIN && temp <= BD_BREW_TEMP_MAX)) return 0;
  static const char ACT[] = "c_upd_act_prof";
  const size_t hdr_max = 2 + (sizeof ACT - 1) + 1 + 5;
  if (cap < hdr_max + 16) return 0;
  Wr w = {out + hdr_max, out + cap, true};
  Rd r = {prof, prof + n, true};
  uint32_t no, wt;
  bool done = false;
  while (next(&r, &no, &wt)) {
    const uint8_t *f0 = r.p;
    skip(&r, wt);
    if (!r.ok) return 0;
    if (no == 4) {                                  // ProfileDto.waterTemperature (float): replaced, phases untouched
      if (wt != 5) return 0;
      if (!done) { w_byte(&w, 0x25); w_f32(&w, temp); done = true; }
      continue;
    }
    if (no > 4 && !done) { w_byte(&w, 0x25); w_f32(&w, temp); done = true; }   // field order, as protobuf-ts
    w_varint(&w, (uint64_t)no << 3 | wt);
    w_bytes(&w, f0, (size_t)(r.p - f0));
  }
  if (!r.ok) return 0;
  if (!done) { w_byte(&w, 0x25); w_f32(&w, temp); }   // only fields 1-3 present (0 is not sent in proto3)
  if (!w.ok) return 0;
  return act_wrap(out, hdr_max, (size_t)(w.p - (out + hdr_max)));
}

bool pb_profile_temp(const uint8_t *b, size_t n, float *temp) {
  *temp = 0;
  Rd r = {b, b + n, true};
  uint32_t no, wt;
  while (next(&r, &no, &wt)) {
    if (no == 4 && wt == 5) *temp = f32(&r);
    else skip(&r, wt);
  }
  return r.ok;
}

bool pb_resp(const uint8_t *b, size_t n, char *act, size_t acap, int *result, char *err, size_t ecap) {
  if (acap) act[0] = 0;
  if (ecap) err[0] = 0;
  *result = 0;
  Rd r = {b, b + n, true};
  uint32_t no, wt;
  while (next(&r, &no, &wt)) {
    if (no == 2 && wt == 0) { *result = (int)varint(&r); continue; }
    if ((no == 1 || no == 3) && wt == 2) {
      const size_t len = (size_t)varint(&r);
      if (!r.ok || (size_t)(r.end - r.p) < len) return false;
      char *o = no == 1 ? act : err;
      const size_t cap = no == 1 ? acap : ecap;
      if (cap) { const size_t k = len < cap - 1 ? len : cap - 1; memcpy(o, r.p, k); o[k] = 0; }
      r.p += len;
      continue;
    }
    skip(&r, wt);
  }
  return r.ok;
}
