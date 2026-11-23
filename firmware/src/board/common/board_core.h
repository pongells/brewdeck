#pragma once
// Board functions outside the core API, used by the core (start-up, logging, the debug server). Each board folder
// (src/board/s3, src/board/p4) implements the ones that differ; board_common.cpp the rest. The core includes this
// header only, never a board's board.h.
#include <stddef.h>
#include <stdint.h>
void board_serial_begin(void);   // the log's serial port (before anything logs)
const char *board_hostname(void);   // the screen's network / mDNS name (each board has its own)
bool board_begin(void);   // panel + touch; false if the touch controller was not found
void board_fake_touch(int x, int y, int ms);   // debug server: a remote press at (x, y) for ms
// debug server (screenshots): n rows of what is on the glass from row y, display width RGB565 pixels each, in the UI's
// landscape orientation; NULL if not available. Valid until the next call (n <= 8).
const uint16_t *board_screen_rows(int y, int n);
// debug server (GET /flash): the flash read mode ("QIO", "DIO", ...) and the flash controller's register
const char *board_flash_mode(uint32_t *ctrl);
// debug server (GET /heap): how flushed pictures reach the panel (e.g. "rotated by the PPA")
const char *board_display_mode(void);
void board_backlight_hz(uint32_t hz);   // debug server (GET /bl): the backlight PWM frequency
// debug server (GET /i2c): every address that answers on the touch controller's I2C bus, one line each, into out;
// returns the length. Call on the UI task (touch reads share the bus).
size_t board_i2c_scan(char *out, size_t cap);
