// ESP32-S3: hold ESP-IDF's cache-sync lock for the whole cache freeze of a flash mapping (ESP-IDF issue #19358).
//
// ESP-IDF freezes the caches whenever it maps flash into the address space (esp_mmu_map: esp_partition_mmap, encrypted
// partition reads, the OTA data read that keeps a new firmware). On the S3 a cache writeback of a range that is not
// cache-line aligned (esp_cache_msync, the ROM's Cache_WriteBack_Addr) sets and clears the same global freeze bit; when
// that happens on the other core during the freeze, the freeze never completes: the interrupt watchdog fires, the panic
// handler stalls on the frozen cache (saved PC in panic_enable_cache) and the hardware watchdog resets the chip.
// The linker sends ESP-IDF's freeze / unfreeze here (--wrap, src/CMakeLists.txt) to take the cache-sync lock first,
// which esp_cache_msync also takes (the lock is recursive on one core: ESP-IDF's own take inside the freeze still works).
// Our own writebacks go through esp_cache_msync for that reason (board_s3.cpp, panel.cpp).
#include <esp_attr.h>
#include "esp_private/esp_cache_private.h"

extern "C" {
void __real_esp_cache_freeze_caches_disable_interrupts(void);
void __real_esp_cache_unfreeze_caches_enable_interrupts(void);

void IRAM_ATTR __wrap_esp_cache_freeze_caches_disable_interrupts(void) {
  esp_cache_sync_ops_enter_critical_section();
  __real_esp_cache_freeze_caches_disable_interrupts();
}

void IRAM_ATTR __wrap_esp_cache_unfreeze_caches_enable_interrupts(void) {
  __real_esp_cache_unfreeze_caches_enable_interrupts();
  esp_cache_sync_ops_exit_critical_section();
}
}
