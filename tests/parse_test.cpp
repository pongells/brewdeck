// REST JSON readers (firmware/src/core/parse.cpp) on real replies from the machine (sim/data). Run via tests/run.sh.
// Args: profiles_all.json profile_8.json profile_1.json status.json
#include <string.h>
#include "parse.h"
#include "test_util.h"

int main(int, char **argv) {
  const char *list = slurp(argv[1]), *p8 = slurp(argv[2]), *p1 = slurp(argv[3]);

  ProfileList l;
  const int n = parse_profile_list(list, &l);
  printf("list: %d\n", n);
  for (int i = 0; i < n; i++) printf("  id=%d sel=%d name='%s'\n", l.p[i].id, l.p[i].selected, l.p[i].name);
  CHECK(n == 9);
  CHECK(l.p[6].id == 8 && l.p[6].selected);
  CHECK(!l.p[0].selected);
  CHECK(strcmp(l.p[0].name, "IUIUIU Classic") == 0);

  ProfileInfo a = {}, b = {};
  CHECK(parse_profile_detail(p8, &a));
  CHECK(parse_profile_detail(p1, &b));
  printf("id8: phases=%d weight=%.1f temp=%.1f\nid1: phases=%d weight=%.1f temp=%.1f\n", a.phases, a.weight, a.temp,
         b.phases, b.weight, b.temp);
  CHECK(a.phases == 19 && a.weight == 21 && a.temp == 90);
  CHECK(b.phases == 4 && b.weight == 36 && b.temp == 93);

  // a truncated detail fails cleanly; a truncated list keeps its complete entries only
  char t[200];
  strncpy(t, p8, sizeof t - 1);
  t[sizeof t - 1] = 0;
  ProfileInfo c = {};
  CHECK(!parse_profile_detail(t, &c));
  char tl[120];
  strncpy(tl, list, sizeof tl - 1);
  tl[sizeof tl - 1] = 0;
  ProfileList l2;
  const int n2 = parse_profile_list(tl, &l2);
  printf("truncated list -> %d\n", n2);
  CHECK(n2 == 2);

  // empty / garbage
  ProfileList l3;
  CHECK(parse_profile_list("[]", &l3) == 0);
  CHECK(parse_profile_list("oops", &l3) == 0);

  LiveData d;
  parse_status("[{\"temperature\":\"90.5\",\"profileName\":\"Leva 9 SINGLE v0.9 (community)\",\"brewActive\":\"false\","
               "\"waterLevel\":\"70\"}]", &d);
  CHECK(d.temperature > 90.4f && d.water_level == 70 && !d.brew_active && strstr(d.profile, "Leva"));
  CHECK(d.readiness == -1);   // not in the reply: the screen falls back to its temperature rule

  LiveData r;   // a real reply: readinessState 3 = READY
  parse_status(slurp(argv[4]), &r);
  CHECK(r.readiness == 3 && r.warmup_remaining == 0);
  return report("parse_test");
}
