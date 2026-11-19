#pragma once
// A "since" timestamp where 0 means "not set". Never write `now | 1` or `now ? now : 1`: that can be 1 ms in the
// future, so an elapsed check in the same millisecond wraps to ~49 days and passes at once. 0 maps to UINT32_MAX,
// 1 ms in the past.
#include <stdint.h>

static inline uint32_t stamp_nz(uint32_t now) { return now ? now : 0xFFFFFFFFu; }
