// Time and waiting on ESP-IDF (esp_timer + FreeRTOS), for the core and the board layers: what Arduino's millis /
// micros / delay / delayMicroseconds did, under names of our own.
#pragma once
#include <stdint.h>
#include <esp_timer.h>
#include <esp_rom_sys.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

// ms / us since boot; uint32_t wraps after ~49 days / ~72 min: compare with "now - then < limit", never "now < x"
static inline uint32_t clock_ms(void) { return (uint32_t)(esp_timer_get_time() / 1000); }
static inline uint32_t clock_us(void) { return (uint32_t)esp_timer_get_time(); }
// lets other tasks run; at least one tick (1 ms) when ms > 0
static inline void wait_ms(uint32_t ms) { vTaskDelay(pdMS_TO_TICKS(ms)); }
static inline void wait_us(uint32_t us) { esp_rom_delay_us(us); }   // busy wait: short pauses only
