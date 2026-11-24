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

#ifdef BREWDECK_DEBUG_HTTP
static void ring_put(const uint8_t *b, size_t n);
#else
static inline void ring_put(const uint8_t *, size_t) {}
#endif

size_t TsLog::write(const uint8_t *b, size_t n) {
  static SemaphoreHandle_t mtx = xSemaphoreCreateMutex();   // UI loop, net task and WS task all log
  if (mtx) xSemaphoreTake(mtx, portMAX_DELAY);
  size_t i = 0;
  while (i < n) {
    if (bol_) {
      char p[24];
      stamp(p, sizeof p);
      Serial.write((const uint8_t *)p, strlen(p));
      ring_put((const uint8_t *)p, strlen(p));
      bol_ = false;
    }
    size_t j = i;
    while (j < n && b[j] != '\n') j++;
    if (j < n) { j++; bol_ = true; }               // up to and including the '\n'
    Serial.write(b + i, j - i);
    ring_put(b + i, j - i);
    i = j;
  }
  if (mtx) xSemaphoreGive(mtx);
  return n;
}

#ifdef BREWDECK_DEBUG_HTTP
#include <esp_heap_caps.h>
#include <esp_attr.h>
#include <esp_log.h>

// ---- the log in memory (callers hold TsLog's mutex, or the IDF hook takes it) ----
static const size_t RING = 256 * 1024;
static char *g_ring = NULL;             // PSRAM, allocated on the first line
static volatile uint32_t g_wr = 0;      // bytes written since boot
static portMUX_TYPE g_ring_mux = portMUX_INITIALIZER_UNLOCKED;
static const uint32_t RTC_MAGIC = 0x4C6F6731;   // "Log1"
static const size_t RTC_N = 3072;
struct RtcTail { uint32_t magic; uint32_t wr; char b[RTC_N]; };
RTC_NOINIT_ATTR static RtcTail g_rtc;   // kept over a crash / watchdog restart
static char *g_prev = NULL;             // the tail from before this boot, in order
static size_t g_prev_n = 0;

static void ring_put(const uint8_t *b, size_t n) {
  if (!g_ring) g_ring = (char *)heap_caps_malloc(RING, MALLOC_CAP_SPIRAM);
  portENTER_CRITICAL_SAFE(&g_ring_mux);
  for (size_t i = 0; i < n; i++) {
    if (g_ring) g_ring[(g_wr + i) % RING] = (char)b[i];
    if (g_rtc.magic == RTC_MAGIC) g_rtc.b[(g_rtc.wr + i) % RTC_N] = (char)b[i];
  }
  g_wr += n;
  if (g_rtc.magic == RTC_MAGIC) g_rtc.wr += n;
  portEXIT_CRITICAL_SAFE(&g_ring_mux);
}
uint32_t log_written(void) { return g_wr; }
size_t log_read(uint32_t from, char *out, size_t cap, uint32_t *next) {
  size_t k = 0;
  portENTER_CRITICAL_SAFE(&g_ring_mux);
  const uint32_t wr = g_wr, oldest = wr > RING ? wr - RING : 0;
  if (from < oldest) from = oldest;
  if (g_ring)
    while (from + k < wr && k < cap) { out[k] = g_ring[(from + k) % RING]; k++; }
  portEXIT_CRITICAL_SAFE(&g_ring_mux);
  if (next) *next = from + k;
  return k;
}
size_t log_before_restart(char *out, size_t cap) {
  const size_t n = g_prev_n < cap ? g_prev_n : cap;
  if (g_prev) memcpy(out, g_prev + (g_prev_n - n), n);
  return n;
}
// ESP-IDF's own lines (drivers, Wi-Fi) into the memory log as well; to serial as before
static int idf_vprintf(const char *fmt, va_list ap) {
  char line[256];
  va_list cp;
  va_copy(cp, ap);
  const int n = vsnprintf(line, sizeof line, fmt, cp);
  va_end(cp);
  if (n > 0) ring_put((const uint8_t *)line, (size_t)n < sizeof line ? (size_t)n : sizeof line - 1);
  return vprintf(fmt, ap);
}
void log_begin(void) {
  if (g_rtc.magic == RTC_MAGIC && g_rtc.wr) {      // a restart that kept RTC memory: save the tail in order
    const size_t n = g_rtc.wr < RTC_N ? g_rtc.wr : RTC_N;
    g_prev = (char *)heap_caps_malloc(n, MALLOC_CAP_SPIRAM);
    if (g_prev) {
      for (size_t i = 0; i < n; i++) g_prev[i] = g_rtc.b[(g_rtc.wr - n + i) % RTC_N];
      g_prev_n = n;
    }
  }
  g_rtc.magic = RTC_MAGIC;
  g_rtc.wr = 0;
  esp_log_set_vprintf(idf_vprintf);
}
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
