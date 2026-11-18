// REST JSON readers (firmware/src/core/parse.cpp) on real replies from the machine (sim/data). Run via tests/run.sh.
// args: profiles_all.json profile_8.json profile_1.json
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "parse.h"
static char *slurp(const char *p){FILE*f=fopen(p,"rb");if(!f){perror(p);exit(1);}static char b[3][16384];static int k;char*o=b[k++%3];size_t n=fread(o,1,16383,f);o[n]=0;fclose(f);return o;}
static int fails=0;
#define CHECK(c) do{ if(!(c)){printf("FAIL %s:%d %s\n",__FILE__,__LINE__,#c);fails++;} }while(0)
int main(int, char**argv){
  ProfileList L; int n=parse_profile_list(slurp(argv[1]),&L);
  printf("list: %d\n",n); for(int i=0;i<n;i++) printf("  id=%d sel=%d name='%s'\n",L.p[i].id,L.p[i].selected,L.p[i].name);
  CHECK(n==10); CHECK(L.p[7].id==8 && L.p[7].selected); CHECK(!L.p[0].selected); CHECK(strcmp(L.p[0].name,"IUIUIU Classic")==0);
  ProfileInfo a={},b={}; CHECK(parse_profile_detail(slurp(argv[2]),&a)); CHECK(parse_profile_detail(slurp(argv[3]),&b));
  printf("id8: phases=%d weight=%.1f temp=%.1f\nid1: phases=%d weight=%.1f temp=%.1f\n",a.phases,a.weight,a.temp,b.phases,b.weight,b.temp);
  CHECK(a.phases==19 && a.weight==21 && a.temp==90); CHECK(b.phases==4 && b.weight==36 && b.temp==93);
  // truncated detail must fail cleanly
  char t[200]; strncpy(t,slurp(argv[2]),199); t[199]=0; ProfileInfo c={}; CHECK(!parse_profile_detail(t,&c));
  // truncated list keeps complete entries only
  char tl[120]; strncpy(tl,slurp(argv[1]),119); tl[119]=0; ProfileList L2; int n2=parse_profile_list(tl,&L2); printf("truncated list -> %d\n",n2); CHECK(n2==2);
  // empty / garbage
  ProfileList L3; CHECK(parse_profile_list("[]",&L3)==0); CHECK(parse_profile_list("oops",&L3)==0);
  LiveData d; parse_status("[{\"temperature\":\"90.5\",\"profileName\":\"Leva 9 SINGLE v0.9 (community)\",\"brewActive\":\"false\",\"waterLevel\":\"70\"}]",&d);
  CHECK(d.temperature>90.4f && d.water_level==70 && !d.brew_active && strstr(d.profile,"Leva"));
  CHECK(d.readiness==-1);   // not in the reply: the screen falls back to its temperature rule
  {   // a real reply (docs/status-sample.json): readinessState 3 = READY
    FILE *f=fopen("docs/status-sample.json","rb"); static char js[8192]; size_t n=f?fread(js,1,sizeof js-1,f):0; if(f) fclose(f); js[n]=0;
    LiveData r; parse_status(js,&r); CHECK(n>0 && r.readiness==3 && r.warmup_remaining==0);
  }
  printf(fails? "%d FAILED\n":"all passed\n",fails); return fails!=0;
}
