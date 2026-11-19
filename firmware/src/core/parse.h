#pragma once
// Tiny allocation-free JSON readers for the Gaggiuino REST replies. Pure C++, no Arduino: unit-testable on the host.
#include <stddef.h>
#include "live.h"

void parse_status(const char *js, LiveData *d);                 // GET /api/system/status
int  parse_profile_list(const char *js, ProfileList *out);      // GET /api/profiles/all  -> count (keeps no details)
bool parse_profile_detail(const char *js, ProfileInfo *p);      // GET /api/profile/<id>  -> fills phases/weight/temp
bool parse_profile_shape(const char *js, bd_profile_shape *s);      // GET /api/profile/<id>  -> phases for the preview
int  parse_shots_latest(const char *js);                        // GET /api/shots/latest  -> lastShotId, 0 on error
bool parse_shot(const char *js, bd_shot *t);                  // GET /api/shots/<id>    -> points (thinned to fit)
