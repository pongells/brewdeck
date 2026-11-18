// Stored-shot readers on real machine replies (2026-11-04): GET /api/shots/<id> (parse.cpp) and the WebSocket shot
// history index (pb.cpp, frame from sim/data/ws_idle.bin). Run via tests/run.sh.
// args: shot_298.json shot_297.json ws_idle.bin
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "parse.h"
#include "pb.h"
static int fails = 0;
#define CHECK(c) do { if (!(c)) { printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #c); fails++; } } while (0)
static char *slurp(const char *p, size_t *len = NULL) {
  FILE *f = fopen(p, "rb"); if (!f) { perror(p); exit(1); }
  static char b[2][1 << 20]; static int k; char *o = b[k++ % 2];
  size_t n = fread(o, 1, sizeof b[0] - 1, f); o[n] = 0; fclose(f); if (len) *len = n; return o;
}
static ShotTrace t, u;
int main(int, char **argv) {
  {   // target weight command, built on the machine's own d_act_prof from ws_idle.bin
    FILE *fp = fopen(argv[3], "rb");
    static uint8_t raw[1 << 16];
    const size_t rn = fp ? fread(raw, 1, sizeof raw, fp) : 0;
    if (fp) fclose(fp);
    const uint8_t *prof = NULL;
    size_t pn = 0;
    for (size_t i = 0; i + 4 <= rn;) {
      const uint32_t fl = raw[i] | raw[i + 1] << 8 | raw[i + 2] << 16 | (uint32_t)raw[i + 3] << 24;
      char act[24];
      const uint8_t *d;
      size_t dl;
      if (pb_envelope(raw + i + 4, fl, act, sizeof act, &d, &dl) && !strcmp(act, "d_act_prof")) { prof = d; pn = dl; }
      i += 4 + fl;
    }
    CHECK(prof != NULL);
    uint32_t id0 = 0, id1 = 0;
    char n0[48], n1[48];
    float w0 = 0, w1 = 0;
    CHECK(pb_profile_info(prof, pn, &id0, n0, sizeof n0, &w0));
    printf("active profile %u \"%s\", target %.1f g, %u B\n", (unsigned)id0, n0, w0, (unsigned)pn);
    static uint8_t cmd[8192], cmd2[8192], back[8192];
    const size_t cn = pb_weight_cmd(prof, pn, 18.5f, cmd, sizeof cmd);
    CHECK(cn > 0);
    char act[24];
    const uint8_t *d;
    size_t dl;
    CHECK(pb_envelope(cmd, cn, act, sizeof act, &d, &dl) && !strcmp(act, "c_upd_act_prof"));
    CHECK(pb_profile_info(d, dl, &id1, n1, sizeof n1, &w1));
    CHECK(id1 == id0 && !strcmp(n1, n0) && w1 == 18.5f);
    CHECK(w0 <= 0 || dl == pn);                                        // weight replaced in place: same size
    // only the weight differs: patching back to the old weight gives the same bytes as patching the original
    const size_t bn = pb_weight_cmd(d, dl, w0, back, sizeof back);
    const size_t c2 = pb_weight_cmd(prof, pn, w0, cmd2, sizeof cmd2);
    CHECK(bn > 0 && bn == c2 && !memcmp(back, cmd2, bn));
    {   // with the profile's own weight the command carries the profile byte for byte
      const size_t sn = pb_weight_cmd(prof, pn, w0, cmd2, sizeof cmd2);
      const uint8_t *sd;
      size_t sdl;
      CHECK(sn && pb_envelope(cmd2, sn, act, sizeof act, &sd, &sdl) && sdl == pn && !memcmp(sd, prof, pn));
    }
    CHECK(pb_weight_cmd(prof, pn, 301.0f, cmd, sizeof cmd) == 0);       // out of the web UI's range
    CHECK(pb_weight_cmd(prof, pn, 18.5f, cmd, 64) == 0);                // buffer too small
    CHECK(pb_weight_cmd(prof, pn - 3, 18.5f, cmd, sizeof cmd) == 0);    // truncated profile
    // brew temperature: only ProfileDto.waterTemperature changes; with the profile's own value the bytes are identical
    float t0 = 0, t1 = 0;
    CHECK(pb_profile_temp(prof, pn, &t0));
    printf("active profile brew temperature %.1f C\n", t0);
    const size_t tn = pb_temp_cmd(prof, pn, 94.0f, cmd, sizeof cmd);
    CHECK(tn > 0 && pb_envelope(cmd, tn, act, sizeof act, &d, &dl) && !strcmp(act, "c_upd_act_prof"));
    CHECK(pb_profile_temp(d, dl, &t1) && t1 == 94.0f);
    CHECK(pb_profile_info(d, dl, &id1, n1, sizeof n1, &w1) && id1 == id0 && !strcmp(n1, n0) && w1 == w0);
    CHECK(t0 <= 0 || dl == pn);
    {
      const size_t sn = pb_temp_cmd(prof, pn, t0, cmd2, sizeof cmd2);
      const uint8_t *sd;
      size_t sdl;
      CHECK(t0 < 1 || (sn && pb_envelope(cmd2, sn, act, sizeof act, &sd, &sdl) && sdl == pn && !memcmp(sd, prof, pn)));
    }
    {   // weight then temperature on the result (what ws.cpp does with keep_sent): both survive
      const size_t wn = pb_weight_cmd(prof, pn, 18.5f, cmd2, sizeof cmd2);
      const uint8_t *wd;
      size_t wdl;
      CHECK(wn && pb_envelope(cmd2, wn, act, sizeof act, &wd, &wdl));
      static uint8_t wp[8192];
      memcpy(wp, wd, wdl);
      const size_t t2 = pb_temp_cmd(wp, wdl, 93.0f, cmd, sizeof cmd);
      CHECK(t2 && pb_envelope(cmd, t2, act, sizeof act, &d, &dl));
      CHECK(pb_profile_info(d, dl, &id1, n1, sizeof n1, &w1) && w1 == 18.5f && pb_profile_temp(d, dl, &t1) && t1 == 93.0f);
    }
    {   // profile without the field (proto3 drops 0): it is added, in field order
      static const uint8_t mini[] = {0x0A, 0x01, 'P', 0x30, 0x08};   // name "P", id 8
      const size_t mn = pb_temp_cmd(mini, sizeof mini, 92.0f, cmd, sizeof cmd);
      CHECK(mn && pb_envelope(cmd, mn, act, sizeof act, &d, &dl) && dl == sizeof mini + 5);
      CHECK(d[3] == 0x25 && d[dl - 2] == 0x30 && pb_profile_temp(d, dl, &t1) && t1 == 92.0f);
    }
    CHECK(pb_temp_cmd(prof, pn, 1.0f, cmd, sizeof cmd) != 0);          // 1-105: low values are real profiles
    CHECK(pb_temp_cmd(prof, pn, 59.0f, cmd, sizeof cmd) != 0);
    CHECK(pb_temp_cmd(prof, pn, 106.0f, cmd, sizeof cmd) == 0);
    CHECK(pb_temp_cmd(prof, pn, 0.0f, cmd, sizeof cmd) == 0);           // never switch the heater off
    CHECK(pb_temp_cmd(prof, pn - 3, 94.0f, cmd, sizeof cmd) == 0);      // truncated profile
  }
  {   // scale type from a synthetic GaggiaSettingsDto with fake secrets around it
    static const uint8_t st[] = {
      0x12, 0x0A, 0x52, 0x08, 'S', 'E', 'C', 'R', 'E', 'T', '!', '!',          // 2: system { 10: "SECRET!!" }
      0x22, 0x10, 0x10, 0x01, 0x18, 0xE8, 0x07, 0x28, 0x01,                     // 4: scales { hw 1, F1 1000, bt 1,
                  0x3A, 0x07, 'A', 'A', ':', 'B', 'B', ':', 'C',                 //   7: pinned MAC }
      0x42, 0x02, 0x08, 0x01,                                                   // 8: webUi { configured }
    };
    WsScaleCfg c;
    CHECK(pb_scale_cfg(st, sizeof st, &c) && c.valid && c.hw_enabled && c.bt_enabled && !c.force_predictive);
    const uint8_t only_bt[] = {0x22, 0x02, 0x28, 0x01};
    CHECK(pb_scale_cfg(only_bt, sizeof only_bt, &c) && c.bt_enabled && !c.hw_enabled);
    const uint8_t pred[] = {0x22, 0x02, 0x08, 0x01};
    CHECK(pb_scale_cfg(pred, sizeof pred, &c) && c.force_predictive);
    CHECK(!pb_scale_cfg(st, 15, &c));                                           // cut inside the scales message
    const uint8_t none[] = {0x42, 0x02, 0x08, 0x01};
    CHECK(!pb_scale_cfg(none, sizeof none, &c) && !c.valid);                     // no scales field
    // steam set point: boiler { 1: 155, 2: offset 7, 3: hpwr 38 } before the fake secrets above
    static const uint8_t bo[] = {0x0A, 0x07, 0x08, 0x9B, 0x01, 0x10, 0x07, 0x18, 0x26,
                                 0x12, 0x0A, 0x52, 0x08, 'S', 'E', 'C', 'R', 'E', 'T', '!', '!'};
    uint32_t deg;
    CHECK(pb_steam_set(bo, sizeof bo, &deg) && deg == 155);
    CHECK(!pb_steam_set(st, sizeof st, &deg) && deg == 0);                       // no boiler field
    CHECK(!pb_steam_set(bo, 5, &deg));                                          // cut inside the boiler message
    // versions { 1: "core", 2: "front1", 3: "web\x01" } after the fake secrets: only 2 and 3, printable only
    static const uint8_t ve[] = {0x12, 0x0A, 0x52, 0x08, 'S', 'E', 'C', 'R', 'E', 'T', '!', '!',
                                 0x3A, 0x14, 0x0A, 0x04, 'c', 'o', 'r', 'e', 0x12, 0x06, 'f', 'r', 'o', 'n', 't', '1',
                                 0x1A, 0x04, 'w', 'e', 'b', 0x01};
    char fv[8], sv[8];
    CHECK(pb_versions(ve, sizeof ve, fv, sizeof fv, sv, sizeof sv) && !strcmp(fv, "front1") && !strcmp(sv, "web"));
    char f3[4];
    CHECK(pb_versions(ve, sizeof ve, f3, sizeof f3, sv, sizeof sv) && !strcmp(f3, "fro"));   // cut to the buffer
    CHECK(!pb_versions(bo, sizeof bo, fv, sizeof fv, sv, sizeof sv) && !fv[0] && !sv[0]);   // no versions field
    CHECK(!pb_versions(ve, 20, fv, sizeof fv, sv, sizeof sv));                              // cut inside versions
  }
  {   // the tare command decodes to exactly the web UI's message
    char act[24];
    const uint8_t *d;
    size_t dl;
    CHECK(PB_TARE_CMD_LEN == 17);
    CHECK(pb_envelope(PB_TARE_CMD, PB_TARE_CMD_LEN, act, sizeof act, &d, &dl) && !strcmp(act, "c_tare_pend"));
    CHECK(dl == 2 && d[0] == 0x10 && d[1] == 0x01);   // UpdateSystemStateCommandDto { tarePending (2) = true }
  }
  {   // flush commands: c_opmode { operationMode }, only FLUSH / FLUSH_AUTO / HOME
    uint8_t b[32];
    char act[24];
    const uint8_t *d;
    size_t dl;
    const int ok[3] = {PB_MODE_FLUSH, PB_MODE_FLUSH_AUTO, PB_MODE_HOME};
    for (int m : ok) {
      const size_t n = pb_opmode_cmd(m, b, sizeof b);
      CHECK(n == 14 && pb_envelope(b, n, act, sizeof act, &d, &dl) && !strcmp(act, "c_opmode"));
      CHECK(dl == 2 && d[0] == 0x08 && d[1] == m);
      CHECK(ws_mode_name(m)[0]);
    }
    CHECK(!strcmp(ws_mode_name(PB_MODE_FLUSH), "FLUSH") && !strcmp(ws_mode_name(PB_MODE_FLUSH_AUTO), "FLUSH_AUTO") &&
          !strcmp(ws_mode_name(PB_MODE_HOME), "HOME"));
    for (int m = -1; m <= 10; m++)   // steam, descale, manual, calibration, power off, brew ...: never built
      if (m != 2 && m != 5 && m != 7) CHECK(pb_opmode_cmd(m, b, sizeof b) == 0);
    CHECK(pb_opmode_cmd(PB_MODE_FLUSH, b, 13) == 0 && pb_opmode_cmd(PB_MODE_FLUSH, NULL, 32) == 0);
  }
  CHECK(parse_shots_latest("[{\"lastShotId\":298}]") == 298);
  CHECK(parse_shots_latest("oops") == 0);
  // shot 298: 21.8 g in 7.3 s (scale on), 47 points
  size_t len;
  char *js = slurp(argv[1], &len);
  CHECK(parse_shot(js, &t));
  printf("shot %d: %d points, %.1f s, '%s', last t %u p %u f %u w %u temp %u tp %u tf %u ph %u\n", t.id, t.n,
         t.dur10 / 10.0, t.profile, t.pt[t.n - 1].t, t.pt[t.n - 1].p, t.pt[t.n - 1].f, t.pt[t.n - 1].w,
         t.pt[t.n - 1].temp, t.pt[t.n - 1].tp, t.pt[t.n - 1].tf, t.pt[t.n - 1].ph);
  CHECK(t.ts == 1793808392u);
  CHECK(t.id == 298 && t.n == 47 && t.dur10 == 73 && !strcmp(t.profile, "Leva 9 SINGLE v0.9 (community)"));
  CHECK(t.pt[0].t == 4 && t.pt[0].p == 5 && t.pt[0].f == 22 && t.pt[0].temp == 897 && t.pt[0].tp == 15);
  CHECK(t.pt[46].t == 73 && t.pt[46].w == 218 && t.pt[46].temp == 900 && t.pt[46].ph == 5);
  for (int i = 1; i < t.n; i++) CHECK(t.pt[i].t > t.pt[i - 1].t);
  // shot 297 (no scale): 139 points
  CHECK(parse_shot(slurp(argv[2]), &u));
  printf("shot %d: %d points, %.1f s, last w %u ph %u\n", u.id, u.n, u.dur10 / 10.0, u.pt[u.n - 1].w, u.pt[u.n - 1].ph);
  CHECK(u.id == 297 && u.n == 139 && u.dur10 == 214 && u.pt[u.n - 1].ph == 13);
  // every truncation fails cleanly (or still parses when only the profile tail is cut)
  char *cut = (char *)malloc(len + 1);
  int ok_cuts = 0;
  for (size_t k = 0; k < len; k++) {
    memcpy(cut, js, k); cut[k] = 0;
    if (parse_shot(cut, &u)) { ok_cuts++; CHECK(u.n == 47); }
  }
  printf("truncations that still parse: %d of %zu\n", ok_cuts, len);
  free(cut);
  CHECK(!parse_shot("{\"datapoints\":{\"timeInShot\":[]}}", &u));
  CHECK(!parse_shot("{\"datapoints\":{\"timeInShot\":[1,2],\"pressure\":[1]}}", &u));   // unequal lengths
  // long shot: thinned to SHOT_MAX
  {
    const int N = 2000;
    static char big[200000]; size_t o = 0;
    o += snprintf(big + o, sizeof big - o, "{\"id\":7,\"duration\":%d,\"datapoints\":{", N);
    static const char *keys[] = {"timeInShot", "pressure", "pumpFlow", "shotWeight", "temperature", "targetPressure", "targetPumpFlow", "phaseIndex"};
    for (int k = 0; k < 8; k++) {
      o += snprintf(big + o, sizeof big - o, "%s\"%s\":[", k ? "," : "", keys[k]);
      for (int i = 0; i < N; i++) o += snprintf(big + o, sizeof big - o, "%s%d", i ? "," : "", k == 0 ? i + 1 : k);
      o += snprintf(big + o, sizeof big - o, "]");
    }
    snprintf(big + o, sizeof big - o, "}}");
    CHECK(parse_shot(big, &u));
    printf("long shot: %d -> %d points, last t %u\n", N, u.n, u.pt[u.n - 1].t);
    CHECK(u.n <= SHOT_MAX && u.n >= SHOT_MAX / 2 && u.pt[0].t == 1 && u.pt[1].t == 4 && u.pt[0].ph == 7);
  }
  // history index from the recorded connect burst
  size_t n;
  const uint8_t *b = (const uint8_t *)slurp(argv[3], &n);
  int found = 0;
  for (size_t i = 0; i + 4 <= n;) {
    uint32_t L = b[i] | b[i + 1] << 8 | b[i + 2] << 16 | (uint32_t)b[i + 3] << 24; const uint8_t *fr = b + i + 4; i += 4 + L;
    char act[24]; const uint8_t *d; size_t dl;
    if (!pb_envelope(fr, L, act, sizeof act, &d, &dl) || strcmp(act, "d_shot_hist_index")) continue;
    WsShotIndex ix;
    CHECK(pb_shot_index(d, dl, &ix));
    found++;
    const char *leva = "Leva 9 SINGLE v0.9 (community)", *lmd = "LMD Single v1.1 (community)";
    printf("index: %d records, first id %u dur %u, last id %u\n", ix.n, ix.r[0].id, ix.r[0].dur10, ix.r[ix.n - 1].id);
    CHECK(ix.n == 50 && ix.r[0].id == 294 && ix.r[0].dur10 == 255 && ix.r[1].id == 293 && ix.r[49].id == 245);
    CHECK(ix.r[0].name_hash == shot_name_hash(leva, strlen(leva)) && ix.r[49].name_hash == shot_name_hash(lmd, strlen(lmd)));
    CHECK(ix.r[0].name_hash != ix.r[49].name_hash);
    printf("index: first ts %u, name \"%s\"; last \"%s\"\n", ix.r[0].ts, ix.r[0].name, ix.r[49].name);
    CHECK(!strcmp(ix.r[0].name, leva) && !strcmp(ix.r[49].name, lmd));   // shot history list shows these
    CHECK(ix.r[0].ts > 1700000000u && ix.r[0].ts > ix.r[49].ts);          // newest first
    for (size_t k = 0; k < dl; k++) { WsShotIndex y; pb_shot_index(d, k, &y); }   // truncations: no crash
  }
  CHECK(found == 1);
  {   // notifications: the recorded shot has 258 x WARN "Fill the water tank!" (a Gaggiuino quirk)
    const uint8_t *s = (const uint8_t *)slurp(argv[4], &n);
    int nn = 0, ok = 0;
    for (size_t i = 0; i + 4 <= n;) {
      uint32_t L = s[i] | s[i + 1] << 8 | s[i + 2] << 16 | (uint32_t)s[i + 3] << 24; const uint8_t *fr = s + i + 4; i += 4 + L;
      char act[24]; const uint8_t *d; size_t dl;
      if (!pb_envelope(fr, L, act, sizeof act, &d, &dl) || strcmp(act, "d_notif")) continue;
      nn++;
      WsNotif nt;
      if (pb_notif(d, dl, &nt) && nt.type == NOTIF_WARN && !strcmp(nt.msg, "Fill the water tank!")) ok++;
      for (size_t k = 0; k < dl; k++) { WsNotif y; pb_notif(d, k, &y); }   // truncations: no crash
    }
    printf("notifications: %d, %d decoded as WARN \"Fill the water tank!\"\n", nn, ok);
    CHECK(nn == 258 && ok == nn);
    const uint8_t hand[] = {0x08, 0x03, 0x12, 0x14, 'R', 'e', 'l', 'e', 'a', 's', 'i', 'n', 'g', ' ', 'p', 'r', 'e', 's', 's',
                            'u', 'r', 'e', '!', 0x07};   // ERROR + text with a control byte (dropped)
    WsNotif h;
    CHECK(pb_notif(hand, sizeof hand, &h) && h.type == NOTIF_ERROR && !strcmp(h.msg, "Releasing pressure!"));
  }
  printf(fails ? "%d FAILED\n" : "all passed\n", fails);
  return fails != 0;
}
