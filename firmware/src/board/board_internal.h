#pragma once
// Board functions outside the core API, used by the core (start-up) and for debug logging.
#include <stddef.h>
#include <stdint.h>
bool board_begin(void);   // panel + touch; false if the touch controller was not found. Call after net_begin().
void board_fake_touch(int x, int y, int ms);   // debug server: a remote press at (x, y) for ms
