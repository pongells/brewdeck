// WebSocket protobuf decoder (firmware/src/core/pb.cpp) on recorded frames (sim/data/ws_idle.bin, 2026-11-04, machine
// idle in HOME), a shot frame and every truncation. Run via tests/run.sh.  args: ws_idle.bin
#include <stdio.h>
#include <string.h>
#include <math.h>
#include <vector>
#include "pb.h"
static int fail = 0;
#define CHECK(c) do { if (!(c)) { printf("FAIL line %d: %s\n", __LINE__, #c); fail++; } } while (0)
static void vput(std::vector<uint8_t> &v, uint64_t x) { while (x >= 0x80) { v.push_back((x & 0x7F) | 0x80); x >>= 7; } v.push_back(x); }
int main(int, char **argv) {
  FILE *f = fopen(argv[1], "rb"); static uint8_t b[1 << 20]; size_t n = fread(b, 1, sizeof b, f); fclose(f);
  int cs = 0, cy = 0, frames = 0; WsSensor ls; WsSystem ly;
  for (size_t i = 0; i + 4 <= n;) {
    uint32_t L = b[i] | b[i+1] << 8 | b[i+2] << 16 | (uint32_t)b[i+3] << 24; const uint8_t *fr = b + i + 4; i += 4 + L; frames++;
    char act[24]; const uint8_t *d; size_t dl;
    CHECK(pb_envelope(fr, L, act, sizeof act, &d, &dl));
    if (!strcmp(act, "d_sensor_snap")) { CHECK(pb_sensor(d, dl, &ls)); cs++; }
    if (!strcmp(act, "d_sys_state")) { CHECK(pb_system(d, dl, &ly)); cy++; }
    for (size_t k = 0; k < L; k++) { char a2[24]; const uint8_t *d2; size_t l2; WsSensor s2; WsSystem y2; WsShot h2;   // truncations: no crash
      if (pb_envelope(fr, k, a2, sizeof a2, &d2, &l2) && d2) { pb_sensor(d2, l2, &s2); pb_system(d2, l2, &y2); pb_shot(d2, l2, &h2); } }
  }
  printf("%d frames, sensor %d, system %d\n", frames, cs, cy);
  printf("last sensor: T %.3f P %.4f W %.1f water %d brew %d\n", ls.temperature, ls.pressure, ls.weight, ls.water_level, ls.brew_active);
  printf("last system: mode %d %s scales %d tare %d persisted %u ready %u warmup %u\n", ly.mode, ws_mode_name(ly.mode), ly.scales_present, ly.tare_pending, ly.persisted_profile, ly.readiness, ly.warmup_remaining);
  CHECK(cs == 51 && cy == 41);
  CHECK(fabsf(ls.temperature - 89.2810f) < 0.001f && fabsf(ls.pressure - 0.41321f) < 0.0001f && ls.water_level == 20);
  CHECK(ly.mode == 7 && !strcmp(ws_mode_name(ly.mode), "HOME") && ly.tare_pending && ly.persisted_profile == 8 && ly.readiness == 3 && !ly.scales_present);
  // shot snapshot built from the 2026-11-04 shot's last frame
  std::vector<uint8_t> s; uint32_t vals[12] = {0, 255, 66, 10, 13, 900, 211, 518, 900, 15, 67, 11};
  for (int no = 1; no <= 11; no++) { vput(s, no << 3); vput(s, vals[no]); }
  vput(s, (99 << 3) | 2); vput(s, 3); s.push_back(1); s.push_back(2); s.push_back(3);   // unknown field: skipped
  std::vector<uint8_t> e; const char *a = "d_shot_snap";
  vput(e, 1 << 3 | 2); vput(e, strlen(a)); e.insert(e.end(), a, a + strlen(a)); vput(e, 2 << 3 | 2); vput(e, s.size()); e.insert(e.end(), s.begin(), s.end());
  char act[24]; const uint8_t *d; size_t dl; WsShot h;
  CHECK(pb_envelope(e.data(), e.size(), act, sizeof act, &d, &dl) && !strcmp(act, "d_shot_snap"));
  CHECK(pb_shot(d, dl, &h));
  CHECK(h.time10 == 255 && h.pressure10 == 66 && h.pump_flow10 == 10 && h.weight_flow10 == 13 && h.temperature10 == 900 && h.weight10 == 211 && h.water10 == 518 && h.target_temp10 == 900 && h.target_flow10 == 15 && h.target_pressure10 == 67 && h.phase == 11);
  CHECK(!pb_shot(d, dl - 2, &h));   // cut inside the unknown field: reported as malformed
  printf(fail ? "FAIL %d\n" : "all checks pass\n", fail); return fail;
}
