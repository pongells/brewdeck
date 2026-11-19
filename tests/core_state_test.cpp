// The core's shot events and command answers (firmware/src/core/core_state.cpp), with the UI side stubbed: which
// events a UI gets for real sequences of machine data. Run via tests/run.sh.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "core_state.h"

#include "test_util.h"

// ---- the UI and board side, recorded ----------------------------------------------------------------------------
static int n_start, n_end, n_point, n_wake, n_answer[8], last_answer[8], n_notice, n_release, last_release;
static bool last_update;
void bd_ui_event(const bd_event *e) {
  if (e->type == BD_EV_NOTICE) { n_notice++; last_update = e->u.notice.update; }
  else if (e->type == BD_EV_PRESSURE_RELEASE) { n_release++; last_release = e->u.release.count; }
  else if (e->type == BD_EV_SHOT_START) n_start++;
  else if (e->type == BD_EV_SHOT_END) n_end++;
  else if (e->type == BD_EV_SHOT_POINT) n_point++;
  else if (e->type == BD_EV_COMMAND) { n_answer[e->u.command.cmd]++; last_answer[e->u.command.cmd] = e->u.command.answer; }
}
void bd_ui_wake(void) { n_wake++; }
void *bd_alloc(size_t n, bd_mem) { return malloc(n); }
void bd_free(void *p) { free(p); }
static const bd_display_info g_di = {800, 480, BD_PIXEL_RGB565, 40};
const bd_display_info *bd_display(void) { return &g_di; }
void bd_display_flush(int, int, int, int, const uint16_t *) {}

static void reset_counts(void) {
  n_start = n_end = n_point = n_wake = n_notice = n_release = 0;
  memset(n_answer, 0, sizeof n_answer);
}
static LiveData live(bool brewing) {
  LiveData d;
  memset(&d, 0, sizeof d);
  d.brew_active = brewing;
  snprintf(d.mode, sizeof d.mode, "%s", "HOME");
  snprintf(d.profile, sizeof d.profile, "%s", "Test");
  snprintf(d.act_profile, sizeof d.act_profile, "%s", "Test");
  d.act_weight = 21.0f;
  d.act_temp = 91.0f;
  return d;
}
static void point(uint32_t t10) {
  WsShot s;
  memset(&s, 0, sizeof s);
  s.time10 = t10;
  s.weight10 = t10 / 2;
  cs_shot_point(&s);
}

int main(void) {
  ProfileList pl;
  memset(&pl, 0, sizeof pl);
  pl.count = 2;
  pl.p[0].id = 7;
  snprintf(pl.p[0].name, sizeof pl.p[0].name, "%s", "Test");
  pl.p[1].id = 8;
  snprintf(pl.p[1].name, sizeof pl.p[1].name, "%s", "Other");
  cs_take_profiles(&pl);
  LiveData d = live(false);
  cs_take_live(&d, BD_LINK_ONLINE);
  uint32_t now = 1000;

  // 1. a normal shot: brewing, points, brewing off -> one START, one END, the whole shot kept
  reset_counts();
  d = live(true); cs_take_live(&d, BD_LINK_ONLINE);
  for (uint32_t t = 1; t <= 200; t++) { point(t); cs_tick(now += 150); }
  d = live(false); cs_take_live(&d, BD_LINK_ONLINE);
  CHECK(n_start == 1 && n_end == 1 && n_point == 200);
  CHECK(bd_live_shot() && bd_live_shot()->n == 200);
  printf("normal shot: start %d end %d points %d kept %d\n", n_start, n_end, n_point, bd_live_shot()->n);

  // 2. the last point arrives after "brewing off" was taken (the order a slow loop pass could see): the shot goes on,
  //    no new START, the points are not wiped
  reset_counts();
  d = live(true); cs_take_live(&d, BD_LINK_ONLINE);
  for (uint32_t t = 1; t <= 100; t++) point(t);
  d = live(false); cs_take_live(&d, BD_LINK_ONLINE);
  point(101);
  for (int i = 0; i < 40; i++) cs_tick(now += 150);   // 6 s of no points: ends (again) once
  CHECK(n_start == 1);
  CHECK(bd_live_shot()->n == 101);
  printf("late point: start %d end %d kept %d\n", n_start, n_end, bd_live_shot()->n);

  // 3. a shot longer than the 720 stored points keeps running (points still coming): no END, no wipe
  reset_counts();
  d = live(true); cs_take_live(&d, BD_LINK_ONLINE);
  for (uint32_t t = 1; t <= 900; t++) { point(t); cs_tick(now += 150); }
  CHECK(n_start == 1 && n_end == 0);
  CHECK(bd_live_shot()->n == BD_SHOT_MAX);
  d = live(false); cs_take_live(&d, BD_LINK_ONLINE);
  CHECK(n_end == 1);
  printf("long shot: start %d end %d kept %d\n", n_start, n_end, bd_live_shot()->n);

  // 4. points stop without "brewing off" (link lost): END after 3 s
  reset_counts();
  for (uint32_t t = 1; t <= 50; t++) point(t);      // time went back: a new shot
  for (int i = 0; i < 30; i++) cs_tick(now += 150);
  CHECK(n_start == 1 && n_end == 1);
  printf("points stop: start %d end %d\n", n_start, n_end);

  // 5. answers: a stop weight the machine takes -> OK; one it never takes -> NOT_TAKEN after 7 s
  reset_counts();
  cs_watch_weight(215);
  cs_tick(now += 150);
  CHECK(n_answer[BD_CMD_STOP_WEIGHT] == 0);         // not reported yet
  d = live(false); d.act_weight = 21.5f; cs_take_live(&d, BD_LINK_ONLINE);
  cs_tick(now += 150);
  CHECK(n_answer[BD_CMD_STOP_WEIGHT] == 1 && last_answer[BD_CMD_STOP_WEIGHT] == BD_ANSWER_OK);
  cs_watch_weight(300);
  for (int i = 0; i < 60; i++) cs_tick(now += 150);
  CHECK(n_answer[BD_CMD_STOP_WEIGHT] == 2 && last_answer[BD_CMD_STOP_WEIGHT] == BD_ANSWER_NOT_TAKEN);
  printf("answers: weight %d (last %d)\n", n_answer[BD_CMD_STOP_WEIGHT], last_answer[BD_CMD_STOP_WEIGHT]);

  // 6. checks: busy while brewing; another profile active -> PROFILE_CHANGED
  d = live(true); cs_take_live(&d, BD_LINK_ONLINE);
  CHECK(cs_check_idle() == BD_ERR_BUSY);
  d = live(false); cs_take_live(&d, BD_LINK_ONLINE);
  CHECK(cs_check_idle() == BD_OK);
  d = live(false); snprintf(d.act_profile, sizeof d.act_profile, "%s", "Other"); cs_take_live(&d, BD_LINK_ONLINE);
  CHECK(cs_check_active(7) == BD_ERR_PROFILE_CHANGED);
  d = live(false); cs_take_live(&d, BD_LINK_NO_MACHINE);
  CHECK(cs_check_idle() == BD_ERR_OFFLINE);

  // 7. the commands' whole rules (cs_cmd_*): ranges refused before anything is watched or sent
  d = live(false); cs_take_live(&d, BD_LINK_ONLINE);
  {
    const char *nm = NULL;
    int w10 = -1, pm = -1;
    CHECK(cs_cmd_temp(7, 0, &nm) == BD_ERR_RANGE);       // heater off: never
    CHECK(cs_cmd_temp(7, 106, &nm) == BD_ERR_RANGE);
    CHECK(cs_cmd_weight(7, 200.1f, &w10, &nm) == BD_ERR_RANGE);
    CHECK(cs_cmd_weight(7, -1.0f, &w10, &nm) == BD_ERR_RANGE);
    CHECK(cs_cmd_weight(7, 21.5f, &w10, &nm) == BD_OK && w10 == 215 && nm && !strcmp(nm, d.act_profile));
    CHECK(cs_cmd_temp(7, 1, &nm) == BD_OK);
    CHECK(cs_cmd_weight(99, 20.0f, &w10, &nm) == BD_ERR_UNKNOWN_ID);
    CHECK(cs_cmd_flush(BD_FLUSH_TIMED, &pm) == BD_OK && pm == PB_MODE_FLUSH_AUTO);
    CHECK(cs_cmd_tare() == (d.scales_present ? BD_OK : BD_ERR_NO_SCALE));
    d = live(true); cs_take_live(&d, BD_LINK_ONLINE);
    CHECK(cs_cmd_temp(7, 93, &nm) == BD_ERR_BUSY);
    CHECK(cs_cmd_flush(BD_FLUSH_ON, &pm) == BD_ERR_BUSY);
  }

  // 8. profile pick: unknown id, one at a time, answered NOT_TAKEN when the machine never reports it (5 s)
  d = live(false); cs_take_live(&d, BD_LINK_ONLINE);
  {
    const char *nm = NULL;
    reset_counts();
    CHECK(cs_cmd_select(99, &nm) == BD_ERR_UNKNOWN_ID);
    CHECK(cs_cmd_select(8, &nm) == BD_OK && nm && !strcmp(nm, "Other"));
    CHECK(cs_cmd_select(7, &nm) == BD_ERR_PENDING);
    for (int i = 0; i < 40; i++) cs_tick(now += 150);
    CHECK(n_answer[BD_CMD_SELECT] == 1 && last_answer[BD_CMD_SELECT] == BD_ANSWER_NOT_TAKEN);
    CHECK(!cs_select_pending());
    d = live(false); cs_take_live(&d, BD_LINK_NO_MACHINE);
    CHECK(cs_cmd_select(7, &nm) == BD_ERR_OFFLINE);
  }

  // 9. flush: OK once the machine reports the mode; tare: NOT_TAKEN when the scale never reads ~0
  d = live(false); cs_take_live(&d, BD_LINK_ONLINE);
  reset_counts();
  cs_watch_flush("FLUSH");
  cs_tick(now += 150);
  d = live(false); snprintf(d.mode, sizeof d.mode, "%s", "FLUSH"); cs_take_live(&d, BD_LINK_ONLINE);
  cs_tick(now += 150);
  CHECK(n_answer[BD_CMD_FLUSH] == 1 && last_answer[BD_CMD_FLUSH] == BD_ANSWER_OK);
  d = live(false); d.weight = 30.0f; cs_take_live(&d, BD_LINK_ONLINE);
  cs_watch_tare();
  for (int i = 0; i < 60; i++) cs_tick(now += 150);
  CHECK(n_answer[BD_CMD_TARE] == 1 && last_answer[BD_CMD_TARE] == BD_ANSWER_NOT_TAKEN);

  // 10. machine texts: the pressure release becomes its own event, the update notice is marked, the tank is dropped
  reset_counts();
  cs_notice(0, "Dropping beats in: 2");
  CHECK(n_release == 1 && last_release == 2 && n_notice == 0);
  cs_notice(0, "Releasing pressure!");
  CHECK(n_release == 2 && last_release == -1);
  cs_notice(0, "Release c929939 now available for download!");
  CHECK(n_notice == 1 && last_update);
  cs_notice(1, "Fill the water tank!");
  CHECK(n_notice == 1);

  return report("core_state_test");
}
