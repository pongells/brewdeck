#include "tlog.h"
#include <sys/time.h>
#include <time.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>

TsLog Log;

static void stamp(char *o, size_t n) {
  struct timeval tv;
  gettimeofday(&tv, NULL);
  if (tv.tv_sec > 1700000000) {                     // the clock is set (NTP)
    struct tm tm;
    localtime_r(&tv.tv_sec, &tm);
    snprintf(o, n, "%02d:%02d:%02d.%03ld ", tm.tm_hour, tm.tm_min, tm.tm_sec, (long)(tv.tv_usec / 1000));
  } else {
    const uint32_t ms = millis();
    snprintf(o, n, "up %lu.%03lu ", (unsigned long)(ms / 1000), (unsigned long)(ms % 1000));
  }
}

size_t TsLog::write(const uint8_t *b, size_t n) {
  static SemaphoreHandle_t mtx = xSemaphoreCreateMutex();   // UI loop, net task and WS task all log
  if (mtx) xSemaphoreTake(mtx, portMAX_DELAY);
  size_t i = 0;
  while (i < n) {
    if (bol_) {
      char p[24];
      stamp(p, sizeof p);
      Serial.write((const uint8_t *)p, strlen(p));
      bol_ = false;
    }
    size_t j = i;
    while (j < n && b[j] != '\n') j++;
    if (j < n) { j++; bol_ = true; }               // up to and including the '\n'
    Serial.write(b + i, j - i);
    i = j;
  }
  if (mtx) xSemaphoreGive(mtx);
  return n;
}

#ifdef BREWDECK_DEBUG_HTTP
#include <esp_heap_caps.h>
void mem_watch(const char *where, const char *what) {
  static volatile uint32_t last = 0;               // 0: not seen yet (the first call only takes the starting point)
  const uint32_t lo = (uint32_t)heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL);
  const uint32_t was = last;
  if (was && lo + 512 > was) return;
  last = lo;
  if (!was) return;
  Log.printf("mem: internal low %u B (-%u) after %s%s%s; free now %u\n", (unsigned)lo, (unsigned)(was - lo), where,
             what ? " " : "", what ? what : "", (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL));
}
#endif
