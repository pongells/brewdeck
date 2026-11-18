#pragma once
#include <stdint.h>
// A "since" timestamp where 0 means "not set". Never write `now | 1`: on an even tick that is 1 ms in the FUTURE,
// so an elapsed check in the same millisecond wraps to ~49 days and passes at once (2026-11-06: the screen went
// off 5 s after the offline screen appeared instead of after 2 min). 0 maps to UINT32_MAX = 1 ms in the past.
static inline uint32_t stamp_nz(uint32_t now) { return now ? now : 0xFFFFFFFFu; }
