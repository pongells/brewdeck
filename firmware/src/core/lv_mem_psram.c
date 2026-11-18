// LVGL 9's heap (LV_USE_STDLIB_MALLOC LV_STDLIB_CUSTOM, lv_conf.h): objects, styles and draw buffers in PSRAM, since
// internal RAM is needed by Wi-Fi / lwIP (with the review screen's objects in internal RAM, free internal RAM once
// fell to ~4 KB after a shot and the WebSocket kept stalling). Falls back to internal RAM if PSRAM is full.
// On the desktop (emulator, renders): plain malloc.
#include "lvgl.h"
#include <stdlib.h>
#ifndef LV_SIM_HOST
#include <esp_heap_caps.h>
#endif

void lv_mem_init(void) {}
void lv_mem_deinit(void) {}
lv_mem_pool_t lv_mem_add_pool(void *mem, size_t bytes) { (void)mem; (void)bytes; return NULL; }
void lv_mem_remove_pool(lv_mem_pool_t pool) { (void)pool; }
void *lv_malloc_core(size_t size) {
#ifndef LV_SIM_HOST
  void *p = heap_caps_malloc(size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  return p ? p : malloc(size);
#else
  return malloc(size);
#endif
}
void *lv_realloc_core(void *p, size_t new_size) {
#ifndef LV_SIM_HOST
  void *q = heap_caps_realloc(p, new_size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  return q ? q : (new_size ? realloc(p, new_size) : NULL);
#else
  return realloc(p, new_size);
#endif
}
void lv_free_core(void *p) { free(p); }
void lv_mem_monitor_core(lv_mem_monitor_t *mon_p) { (void)mon_p; }
lv_result_t lv_mem_test_core(void) { return LV_RESULT_OK; }
