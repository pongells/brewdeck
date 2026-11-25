#pragma once
// Debug builds: sends every log line to a log server on the network, and watches the UI loop.
// - Log server: UDP, one line per datagram, syslog RFC 5424 ("<14>1 - brewdeck-p4 brewdeck - - - 14:02:43.512 ...":
//   no timestamp field, the receiver stamps it; the screen's time starts the text), for VictoriaLogs
//   (tools/victorialogs), rsyslog or tools/logserver.py. Target: LOG_HOST / LOG_PORT in wifi_config.h, or GET /loghost?to=
//   (saved on the screen; empty = off). Lines written while Wi-Fi is down are sent when it is back, as long as they
//   are still in the 256 KB memory log (tlog.h).
// - UI loop: when bd_core_loop has not come round for 5 s, logs where it stopped (the section and the board's last
//   flush), from this task, so the line reaches the log server while the UI is stuck; and again when it runs.
#include <stdint.h>

void logsend_begin(void);                   // after Wi-Fi is started
void loop_mark(const char *where);          // bd_core_loop: the section it is entering (a string literal)
uint32_t loop_last_ms(void);                // clock_ms() of the last loop_mark (0 = none yet)
const char *logsend_target(void);           // "host:port" or "" (off)
void logsend_set_target(const char *to);    // "host[:port]", "" = off; saved
void logsend_counts(uint32_t *sent, uint32_t *errors, uint32_t *unresolved);   // datagrams since boot
