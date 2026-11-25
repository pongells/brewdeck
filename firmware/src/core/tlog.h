#pragma once
// The log, to the board's serial port (board_log_write) with a time in front of every line: "14:02:43.512 " once NTP
// has set the clock (CET/CEST, as the idle clock), "up 63.204 " (seconds since boot) before that. Use Log.printf /
// Log.println. Lines of ESP-IDF and the Arduino core keep their own stamps.
#include <stddef.h>
#include <stdint.h>

class TsLog {
 public:
  void printf(const char *fmt, ...) __attribute__((format(printf, 2, 3)));
  void println(const char *s);
  void write(const char *b, size_t n);   // whole or partial lines
 private:
  bool bol_ = true;     // at the beginning of a line
};
extern TsLog Log;

// Debug builds: logs when the internal-RAM low point (heap_caps_get_minimum_free_size) has dropped by 512 bytes or
// more since the last such line, with where it was noticed ("mem: internal low 16744 B (-1536) after ws d_shot_hist_index;
// free now 34120"). Called after each step of the core loop, each WebSocket message, each HTTP request and each
// screenshot, from any task: what is named is where the drop was NOTICED (the cause ran just before, in that task or
// another). Release builds: nothing.
#ifdef BREWDECK_DEBUG_HTTP
// Debug builds keep the log in memory too (GET /log, and sent to a log server, debug_http.cpp): the last 256 KB in
// PSRAM, and the last 3 KB in RTC memory, which survives a crash or watchdog restart (not a power cut or USB flash).
// Positions are bytes written since boot. log_read copies from position `from` (moved up to the oldest kept byte)
// and returns the bytes copied; *next = where to continue.
size_t log_read(uint32_t from, char *out, size_t cap, uint32_t *next);
uint32_t log_written(void);
size_t log_before_restart(char *out, size_t cap);   // the RTC tail from before this boot (0 = none)
void log_begin(void);   // once, early in setup: keeps the RTC tail, catches ESP-IDF's own log lines
void mem_watch(const char *where, const char *what = nullptr);
#else
static inline void mem_watch(const char *, const char * = nullptr) {}
#endif
