// Firmware updates pushed from the computer + keeping or rolling back a new image (see ota.h). Debug builds only.
#include <Arduino.h>
#ifdef BREWDECK_DEBUG_HTTP
#include <WiFi.h>
#include <esp_ota_ops.h>
#include <esp_app_format.h>
#include <string.h>
#include "ota.h"
#include "brewdeck_core.h"
#include "logsend.h"
#include "net.h"
#include "tlog.h"
#if __has_include("ota_token.h")
#include "ota_token.h"
#endif
#ifndef OTA_TOKEN
#define OTA_TOKEN ""
#endif

static const uint32_t KEEP_AFTER_MS = 60000, STALL_MS = 20000, WIFI_WITHIN_MS = 180000;

// Arduino marks a new image as good as soon as it starts; here it is marked only after the checks in watch()
// (needed while the Arduino core runs the start-up)
extern "C" bool verifyRollbackLater() { return true; }

// what the UI shows (BD_EV_UPDATE): written by the receiving task, read by the UI loop (ota_poll)
static portMUX_TYPE g_umux = portMUX_INITIALIZER_UNLOCKED;
static struct {
  bd_update state;
  uint32_t received, total, seq;
  char built[24], from[16];
} g_up = {BD_UPDATE_NONE, 0, 0, 0, "", ""};
static void up_set(bd_update st, uint32_t got, uint32_t total) {
  portENTER_CRITICAL(&g_umux);
  g_up.state = st;
  g_up.received = got;
  g_up.total = total;
  g_up.seq++;
  portEXIT_CRITICAL(&g_umux);
}
bool ota_poll(bd_event *ev) {
  static uint32_t seen = 0, sent_at = 0;
  static char built[24], from[16];
  portENTER_CRITICAL(&g_umux);
  const uint32_t seq = g_up.seq;
  const bd_update st = g_up.state;
  const uint32_t got = g_up.received, total = g_up.total;
  memcpy(built, g_up.built, sizeof built);
  memcpy(from, g_up.from, sizeof from);
  portEXIT_CRITICAL(&g_umux);
  if (seq == seen) return false;
  if (st == BD_UPDATE_RECEIVING && seen && millis() - sent_at < 250) return false;   // progress: 4 per second is plenty
  seen = seq;
  sent_at = millis();
  ev->type = BD_EV_UPDATE;
  ev->u.update.state = st;
  ev->u.update.received = got;
  ev->u.update.total = total;
  ev->u.update.percent = total ? (int)((uint64_t)got * 100 / total) : 0;
  ev->u.update.built = built;
  ev->u.update.from = from;
  return true;
}
// the new image's build date from its app description ("Nov 24 2026", "21:40:12") as "24 Nov 2026 21:40"
static void image_built(const uint8_t *b, size_t n, char *out, size_t cap) {
  out[0] = 0;
  const size_t off = sizeof(esp_image_header_t) + sizeof(esp_image_segment_header_t);
  if (n < off + sizeof(esp_app_desc_t)) return;
  const esp_app_desc_t *d = (const esp_app_desc_t *)(b + off);
  if (d->magic_word != ESP_APP_DESC_MAGIC_WORD) return;
  char mon[4] = "";
  int day = 0, year = 0;
  if (sscanf(d->date, "%3s %d %d", mon, &day, &year) != 3) return;
  snprintf(out, cap, "%d %s %d %.5s", day, mon, year, d->time);
}

static esp_ota_img_states_t run_state(void);
static bool machine_busy(char *why, size_t cap) {
  const bd_machine *m = bd_machine_state();     // read from this task: flags only, a stale value costs one retry
  if (m->link != BD_LINK_ONLINE) return false;  // machine off or unreachable: nothing can be running
  const char *w = m->brewing ? "a shot" : m->steaming || !strcmp(m->mode, "STEAM") ? "steam" : m->hot_water ? "hot water"
                : !strncmp(m->mode, "FLUSH", 5) ? "a flush" : NULL;
  if (w) snprintf(why, cap, "refused: %s is running\n", w);
  return w != NULL;
}

bool ota_receive(WiFiClient &c, size_t len, const char *token, char *msg, size_t cap) {
  if (!OTA_TOKEN[0]) return snprintf(msg, cap, "refused: this build has no OTA_TOKEN (build.sh makes it)\n"), false;
  if (!token || strcmp(token, OTA_TOKEN) != 0) {
    Log.println("ota: refused a push with a wrong password");
    return snprintf(msg, cap, "refused: wrong password\n"), false;
  }
  if (machine_busy(msg, cap)) return false;
  if (run_state() == ESP_OTA_IMG_PENDING_VERIFY)   // the way back must lead to a firmware that passed its trial
    return snprintf(msg, cap, "refused: the running firmware is still on trial (%lu s left); try again then\n",
                    (unsigned long)(millis() < KEEP_AFTER_MS ? (KEEP_AFTER_MS - millis()) / 1000 + 1 : 1)), false;
  const esp_partition_t *next = esp_ota_get_next_update_partition(NULL);
  if (!next || len < sizeof(esp_image_header_t) || len > next->size)
    return snprintf(msg, cap, "refused: image size %u (slot %u)\n", (unsigned)len, next ? (unsigned)next->size : 0), false;

  static uint8_t buf[2048];   // not on the stack: the debug server's task has ~1.8 KB to spare (it overflowed)
  size_t got = 0;
  uint32_t t0 = millis(), last_log = 0;
  bool started = false;
  esp_ota_handle_t oh = 0;
  esp_err_t e = ESP_OK;
  uint32_t flash_us = 0;   // time spent erasing + writing flash (the rest is the network)
  while (got < len) {
    const size_t want = len - got < sizeof buf ? len - got : sizeof buf;
    const size_t n = c.readBytes(buf, want);
    if (!n) break;
    if (!started) {   // the image header: an app image for this chip
      const esp_image_header_t *h = (const esp_image_header_t *)buf;
      if (n < sizeof *h || h->magic != ESP_IMAGE_HEADER_MAGIC || h->chip_id != CONFIG_IDF_FIRMWARE_CHIP_ID)
        return snprintf(msg, cap, "refused: not a firmware image for this chip (chip id %u, this is %u)\n",
                        n >= sizeof *h ? (unsigned)h->chip_id : 0, (unsigned)CONFIG_IDF_FIRMWARE_CHIP_ID), false;
      // the image's flash is erased here in one go, in 64 KB blocks where aligned (sector by sector as the data
      // arrives took ~twice as long); with flash encryption on, the chip encrypts what is written
      const uint32_t e0 = millis();
      e = esp_ota_begin(next, len, &oh);
      flash_us += (millis() - e0) * 1000;
      if (e != ESP_OK) return snprintf(msg, cap, "failed to start: %s\n", esp_err_to_name(e)), false;
      char built[sizeof g_up.built], from[sizeof g_up.from];   // read outside the lock: remoteIP() waits on lwIP
      image_built(buf, n, built, sizeof built);
      snprintf(from, sizeof from, "%s", c.remoteIP().toString().c_str());
      portENTER_CRITICAL(&g_umux);                // only copies inside (interrupts are off here)
      memcpy(g_up.built, built, sizeof built);
      memcpy(g_up.from, from, sizeof from);
      portEXIT_CRITICAL(&g_umux);
      Log.printf("ota: receiving %u bytes into %s\n", (unsigned)len, next->label);
      started = true;
    }
    const uint32_t w0 = micros();
    e = esp_ota_write(oh, buf, n);
    flash_us += micros() - w0;
    if (e != ESP_OK) {
      snprintf(msg, cap, "write failed at %u: %s\n", (unsigned)got, esp_err_to_name(e));
      esp_ota_abort(oh);
      up_set(BD_UPDATE_STOPPED, got, len);
      Log.printf("ota: %s", msg);
      return false;
    }
    got += n;
    up_set(BD_UPDATE_RECEIVING, got, len);
    if (millis() - last_log > 5000) {
      last_log = millis();
      Log.printf("ota: %u %%\n", (unsigned)(got * 100 / len));
    }
  }
  if (got < len) {
    if (started) {
      esp_ota_abort(oh);
      up_set(BD_UPDATE_STOPPED, got, len);
    }
    snprintf(msg, cap, "connection ended after %u of %u bytes; nothing changed\n", (unsigned)got, (unsigned)len);
    Log.printf("ota: %s", msg);
    return false;
  }
  const uint32_t c0 = millis();
  e = esp_ota_end(oh);                                   // checks the whole image (segments, hash)
  if (e == ESP_OK) e = esp_ota_set_boot_partition(next);   // the next boot, on trial (rollback)
  const uint32_t check_ms = millis() - c0;
  if (e != ESP_OK) {
    snprintf(msg, cap, "image check failed: %s; nothing changed\n", esp_err_to_name(e));
    up_set(BD_UPDATE_STOPPED, got, len);
    Log.printf("ota: %s", msg);
    return false;
  }
  up_set(BD_UPDATE_INSTALLED, len, len);
  snprintf(msg, cap, "installed into %s (%u bytes in %.1f s: flash %.1f s, image check %.1f s); restarting, kept if "
           "it runs properly for 60 s\n", next->label, (unsigned)len, (millis() - t0) / 1000.0f, flash_us / 1e6f,
           check_ms / 1000.0f);
  Log.printf("ota: %s", msg);
  return true;
}

void ota_restart(void) {
  delay(2500);   // the screen shows "Update installed" for ~2 s; the last lines reach the log server
  esp_restart();
}

static esp_ota_img_states_t run_state(void) {
  esp_ota_img_states_t st = ESP_OTA_IMG_UNDEFINED;
  esp_ota_get_state_partition(esp_ota_get_running_partition(), &st);
  return st;
}
const char *ota_slot(void) {
  static char b[40];
  const esp_ota_img_states_t st = run_state();
  snprintf(b, sizeof b, "%s (%s)", esp_ota_get_running_partition()->label,
           st == ESP_OTA_IMG_PENDING_VERIFY ? "new, on trial" : st == ESP_OTA_IMG_VALID ? "kept" : "flashed by USB");
  return b;
}

static void watch(void *) {
  for (;;) {
    vTaskDelay(pdMS_TO_TICKS(1000));
    const uint32_t now = millis(), loop = loop_last_ms();
    const bool loop_ok = loop && now - loop < 5000, wifi = net_wifi_up(NULL);
    const char *bad = (!loop && now > STALL_MS) || (loop && now - loop >= STALL_MS) ? "the UI loop stopped"
                    : now >= WIFI_WITHIN_MS && !wifi ? "no Wi-Fi within 3 min" : NULL;
    if (bad) {
      Log.printf("ota: the new firmware does not run properly (%s): going back to the previous one\n", bad);
      delay(1500);
      esp_ota_mark_app_invalid_rollback_and_reboot();
    }
    if (now >= KEEP_AFTER_MS && loop_ok && wifi) {
      esp_ota_mark_app_valid_cancel_rollback();
      Log.printf("ota: new firmware kept (%s ran properly for %u s)\n", esp_ota_get_running_partition()->label,
                 (unsigned)(now / 1000));
      vTaskDelete(NULL);
    }
  }
}

void ota_begin(void) {
  const esp_partition_t *run = esp_ota_get_running_partition();
  if (run_state() != ESP_OTA_IMG_PENDING_VERIFY) {
    Log.printf("ota: running %s\n", ota_slot());
    return;
  }
  Log.printf("ota: running new firmware in %s, on trial for 60 s\n", run->label);
  xTaskCreatePinnedToCore(watch, "ota_watch", 3072, NULL, 2, NULL, 0);
}
#endif  // BREWDECK_DEBUG_HTTP
