// Firmware entry point: everything runs in the Brewdeck core (src/core/core.cpp), which starts the board, the network
// and the UI (Brewdeck's own in src/ui: ui_bridge.cpp, ui.cpp and screens/; another UI implements the same bd_ui_*
// functions) through brewdeck_core.h.
#include <nvs_flash.h>
#include <esp_partition.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include "core_internal.h"

// LVGL 9 draws nested objects recursively (lv_obj_redraw), ~10 levels deep on our screens plus the label renderer:
// 8 KB overflowed (stack canary) a few seconds after boot. Measured peak ~8.2 KB (debug GET /tasks, every screen
// redrawn): 12 KB (internal RAM).
static const uint32_t CORE_STACK = 12 * 1024;

// settings storage (core/kv.cpp, the UI's store): a partition from an older layout or a full one is erased and started
// again, as the Arduino core did (settings are lost then, the screen still starts)
static void nvs_start(void) {
  esp_err_t e = nvs_flash_init();
  if (e == ESP_ERR_NVS_NO_FREE_PAGES || e == ESP_ERR_NVS_NEW_VERSION_FOUND) {
    const esp_partition_t *p = esp_partition_find_first(ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_DATA_NVS, NULL);
    if (p && esp_partition_erase_range(p, 0, p->size) == ESP_OK) e = nvs_flash_init();
  }
  if (e != ESP_OK) printf("nvs: not started (%s): settings are not kept\n", esp_err_to_name(e));
}

// the core's task: its setup, then one loop pass after another (the UI is drawn from here; the network runs in its own
// tasks on core 0)
static void core_task(void *) {
  bd_core_setup();
  for (;;) bd_core_loop();
}

extern "C" void app_main(void) {
  nvs_start();
  xTaskCreatePinnedToCore(core_task, "core", CORE_STACK, NULL, 1, NULL, 1);
}
