// Firmware updates: writing a new image, the update page's events, keeping or rolling back a new image (see ota.h).
#include <esp_ota_ops.h>
#include <esp_app_format.h>
#include <stdio.h>
#include <string.h>
#include "ota.h"
#include "brewdeck_core.h"
#include "net.h"
#include "systime.h"
#include "tlog.h"

static const uint32_t KEEP_AFTER_MS = 60000, STALL_MS = 20000, WIFI_WITHIN_MS = 180000;

// A new image stays "pending verify" until watch() marks it good (ESP-IDF never does by itself); restarted before that,
// the bootloader goes back to the previous one.

// what the UI shows (BD_EV_UPDATE): written by the writing task, read by the UI loop (ota_poll)
static portMUX_TYPE g_umux = portMUX_INITIALIZER_UNLOCKED;
static struct {
  bd_update state;
  uint32_t received, total, seq;
  char built[24], from[24], version[16], title[24];
} g_up = {BD_UPDATE_NONE, 0, 0, 0, "", "", "", ""};
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
  static char built[sizeof g_up.built], from[sizeof g_up.from], version[sizeof g_up.version], title[sizeof g_up.title];
  portENTER_CRITICAL(&g_umux);
  const uint32_t seq = g_up.seq;
  const bd_update st = g_up.state;
  const uint32_t got = g_up.received, total = g_up.total;
  memcpy(built, g_up.built, sizeof built);
  memcpy(from, g_up.from, sizeof from);
  memcpy(version, g_up.version, sizeof version);
  memcpy(title, g_up.title, sizeof title);
  portEXIT_CRITICAL(&g_umux);
  if (seq == seen) return false;
  if (st == BD_UPDATE_RECEIVING && seen && clock_ms() - sent_at < 250) return false;   // progress: 4 per second is plenty
  seen = seq;
  sent_at = clock_ms();
  ev->type = BD_EV_UPDATE;
  ev->u.update.state = st;
  ev->u.update.received = got;
  ev->u.update.total = total;
  ev->u.update.percent = total ? (int)((uint64_t)got * 100 / total) : 0;
  ev->u.update.built = built;
  ev->u.update.from = from;
  ev->u.update.version = version;
  ev->u.update.title = title;
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

static esp_ota_img_states_t run_state(void) {
  esp_ota_img_states_t st = ESP_OTA_IMG_UNDEFINED;
  esp_ota_get_state_partition(esp_ota_get_running_partition(), &st);
  return st;
}
bool ota_on_trial(void) { return run_state() == ESP_OTA_IMG_PENDING_VERIFY; }

bool ota_machine_busy(char *why, size_t cap) {
  const bd_machine *m = bd_machine_state();     // read from another task: flags only, a stale value costs one retry
  if (m->link != BD_LINK_ONLINE) return false;  // machine off or unreachable: nothing can be running
  const char *w = m->brewing ? "a shot" : m->steaming || !strcmp(m->mode, "STEAM") ? "steam" : m->hot_water ? "hot water"
                : !strncmp(m->mode, "FLUSH", 5) ? "a flush" : NULL;
  if (w) snprintf(why, cap, "refused: %s is running\n", w);
  return w != NULL;
}

// the image being written (one at a time)
static struct {
  bool open;
  esp_ota_handle_t h;
  const esp_partition_t *part;
  uint32_t len, got, t0, last_log, flash_us;
} g_w;

bool ota_open(uint32_t len, const uint8_t *first, size_t n, const char *version, const char *title, const char *from,
              char *msg, size_t cap) {
  if (g_w.open) return snprintf(msg, cap, "refused: another update is being written\n"), false;
  if (ota_machine_busy(msg, cap)) return false;
  if (ota_on_trial())   // the way back must lead to a firmware that passed its trial
    return snprintf(msg, cap, "refused: the running firmware is still on trial (%lu s left); try again then\n",
                    (unsigned long)(clock_ms() < KEEP_AFTER_MS ? (KEEP_AFTER_MS - clock_ms()) / 1000 + 1 : 1)), false;
  const esp_partition_t *next = esp_ota_get_next_update_partition(NULL);
  if (!next || len < sizeof(esp_image_header_t) || len > next->size)
    return snprintf(msg, cap, "refused: image size %u (slot %u)\n", (unsigned)len, next ? (unsigned)next->size : 0), false;
  const esp_image_header_t *hd = (const esp_image_header_t *)first;   // an app image for this chip
  if (n < sizeof *hd || hd->magic != ESP_IMAGE_HEADER_MAGIC || hd->chip_id != CONFIG_IDF_FIRMWARE_CHIP_ID)
    return snprintf(msg, cap, "refused: not a firmware image for this chip (chip id %u, this is %u)\n",
                    n >= sizeof *hd ? (unsigned)hd->chip_id : 0, (unsigned)CONFIG_IDF_FIRMWARE_CHIP_ID), false;
  memset(&g_w, 0, sizeof g_w);
  g_w.t0 = clock_ms();
  // the image's flash is erased here in one go, in 64 KB blocks where aligned (sector by sector as the data arrives
  // took ~twice as long); with flash encryption on, the chip encrypts what is written
  const esp_err_t e = esp_ota_begin(next, len, &g_w.h);
  g_w.flash_us = (clock_ms() - g_w.t0) * 1000;
  if (e != ESP_OK) return snprintf(msg, cap, "failed to start: %s\n", esp_err_to_name(e)), false;
  char built[sizeof g_up.built];
  image_built(first, n, built, sizeof built);
  portENTER_CRITICAL(&g_umux);   // only copies inside (interrupts are off here)
  memcpy(g_up.built, built, sizeof built);
  snprintf(g_up.from, sizeof g_up.from, "%s", from ? from : "");
  snprintf(g_up.version, sizeof g_up.version, "%s", version ? version : "");
  snprintf(g_up.title, sizeof g_up.title, "%s", title ? title : "");
  portEXIT_CRITICAL(&g_umux);
  g_w.open = true;
  g_w.part = next;
  g_w.len = len;
  Log.printf("ota: writing %u bytes into %s%s%s\n", (unsigned)len, next->label, version && *version ? ", release " : "",
             version ? version : "");
  return ota_write(first, n, msg, cap);
}

void ota_abort(const char *why) {
  if (!g_w.open) return;
  esp_ota_abort(g_w.h);
  g_w.open = false;
  up_set(BD_UPDATE_STOPPED, g_w.got, g_w.len);
  const size_t n = strlen(why);
  Log.printf("ota: stopped after %u of %u bytes, nothing changed: %s%s", (unsigned)g_w.got, (unsigned)g_w.len, why,
             n && why[n - 1] == '\n' ? "" : "\n");
}

bool ota_write(const uint8_t *b, size_t n, char *msg, size_t cap) {
  if (!g_w.open) return snprintf(msg, cap, "write without an open image\n"), false;
  if (g_w.got + n > g_w.len) {
    snprintf(msg, cap, "more data than the image size (%u)\n", (unsigned)g_w.len);
    ota_abort(msg);
    return false;
  }
  const uint32_t w0 = clock_us();
  const esp_err_t e = esp_ota_write(g_w.h, b, n);
  g_w.flash_us += clock_us() - w0;
  if (e != ESP_OK) {
    snprintf(msg, cap, "write failed at %u: %s\n", (unsigned)g_w.got, esp_err_to_name(e));
    ota_abort(msg);
    return false;
  }
  g_w.got += n;
  up_set(BD_UPDATE_RECEIVING, g_w.got, g_w.len);
  if (clock_ms() - g_w.last_log > 5000) {
    g_w.last_log = clock_ms();
    Log.printf("ota: %u %%\n", (unsigned)((uint64_t)g_w.got * 100 / g_w.len));
  }
  return true;
}

bool ota_finish(char *msg, size_t cap) {
  if (!g_w.open) return snprintf(msg, cap, "finish without an open image\n"), false;
  if (g_w.got != g_w.len) {
    snprintf(msg, cap, "the image ended after %u of %u bytes; nothing changed\n", (unsigned)g_w.got, (unsigned)g_w.len);
    ota_abort(msg);
    return false;
  }
  const uint32_t c0 = clock_ms();
  esp_err_t e = esp_ota_end(g_w.h);                            // checks the whole image (segments, hash)
  g_w.open = false;                                            // esp_ota_end frees the handle either way
  if (e == ESP_OK) e = esp_ota_set_boot_partition(g_w.part);   // the next boot, on trial (rollback)
  const uint32_t check_ms = clock_ms() - c0;
  if (e != ESP_OK) {
    snprintf(msg, cap, "image check failed: %s; nothing changed\n", esp_err_to_name(e));
    up_set(BD_UPDATE_STOPPED, g_w.got, g_w.len);
    Log.printf("ota: %s", msg);
    return false;
  }
  up_set(BD_UPDATE_INSTALLED, g_w.len, g_w.len);
  snprintf(msg, cap, "installed into %s (%u bytes in %.1f s: flash %.1f s, image check %.1f s); restarting, kept if "
           "it runs properly for 60 s\n", g_w.part->label, (unsigned)g_w.len, (clock_ms() - g_w.t0) / 1000.0f,
           g_w.flash_us / 1e6f, check_ms / 1000.0f);
  Log.printf("ota: %s", msg);
  return true;
}

void ota_restart(void) {
  wait_ms(2500);   // the screen shows "Update installed" for ~2 s; the last lines reach the log server
  esp_restart();
}

const char *ota_slot(void) {
  static char b[40];
  const esp_ota_img_states_t st = run_state();
  snprintf(b, sizeof b, "%s (%s)", esp_ota_get_running_partition()->label,
           st == ESP_OTA_IMG_PENDING_VERIFY ? "new, on trial" : st == ESP_OTA_IMG_VALID ? "kept" : "flashed by USB");
  return b;
}

void ota_recovery_check(void) {
  int x, y;
  if (!bd_touch_read(&x, &y)) return;
  Log.println("ota: the screen is pressed at start: hold 5 s to start the previous firmware");
  const uint32_t t0 = clock_ms();
  uint32_t last = t0;
  while (clock_ms() - t0 < 5000) {
    wait_ms(50);
    if (bd_touch_read(&x, &y)) last = clock_ms();
    else if (clock_ms() - last > 300) {   // let go (short gaps are the touch controller's, not the finger's)
      Log.println("ota: released, starting as usual");
      return;
    }
  }
  const esp_partition_t *other = esp_ota_get_next_update_partition(NULL);
  esp_app_desc_t d;
  esp_ota_img_states_t st = ESP_OTA_IMG_UNDEFINED;
  const bool app = other && esp_ota_get_partition_description(other, &d) == ESP_OK;
  if (other) esp_ota_get_state_partition(other, &st);
  if (!app || st == ESP_OTA_IMG_INVALID || st == ESP_OTA_IMG_ABORTED) {
    Log.println("ota: no other firmware to start (only one was ever installed): starting as usual");
    for (int i = 0; i < 2; i++) { bd_set_backlight(0); wait_ms(150); bd_set_backlight(100); wait_ms(150); }
    return;
  }
  Log.printf("ota: held 5 s: starting the firmware in %s (%s, built %s %s)\n", other->label, d.version, d.date, d.time);
  bd_set_backlight(0);   // one blink: taken
  wait_ms(300);
  bd_set_backlight(100);
  if (esp_ota_set_boot_partition(other) != ESP_OK) {
    Log.println("ota: could not select it: starting as usual");
    return;
  }
  wait_ms(500);
  esp_restart();
}

static volatile uint32_t g_loop_ms = 0;
void ota_loop_alive(void) { g_loop_ms = clock_ms(); }

static void watch(void *) {
  for (;;) {
    vTaskDelay(pdMS_TO_TICKS(1000));
    const uint32_t now = clock_ms(), loop = g_loop_ms;
    const bool loop_ok = loop && now - loop < 5000, wifi = net_wifi_up(NULL);
    const char *bad = (!loop && now > STALL_MS) || (loop && now - loop >= STALL_MS) ? "the UI loop stopped"
                    : now >= WIFI_WITHIN_MS && !wifi ? "no Wi-Fi within 3 min" : NULL;
    if (bad) {
      Log.printf("ota: the new firmware does not run properly (%s): going back to the previous one\n", bad);
      wait_ms(1500);
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
  if (!ota_on_trial()) {
    Log.printf("ota: running %s\n", ota_slot());
    return;
  }
  Log.printf("ota: running new firmware in %s, on trial for 60 s\n", run->label);
  xTaskCreatePinnedToCore(watch, "ota_watch", 3072, NULL, 2, NULL, 0);
}

#ifdef BREWDECK_DEBUG_HTTP
#if __has_include("ota_token.h")
#include "ota_token.h"
#endif
#ifndef OTA_TOKEN
#define OTA_TOKEN ""
#endif

bool ota_receive(TcpConn &c, size_t len, const char *token, char *msg, size_t cap) {
  if (!OTA_TOKEN[0]) return snprintf(msg, cap, "refused: this build has no OTA_TOKEN (build.sh makes it)\n"), false;
  if (!token || strcmp(token, OTA_TOKEN) != 0) {
    Log.println("ota: refused a push with a wrong password");
    return snprintf(msg, cap, "refused: wrong password\n"), false;
  }
  static uint8_t buf[2048];   // not on the stack: the debug server's task has ~1.8 KB to spare (it overflowed)
  size_t got = 0;
  while (got < len) {
    const size_t want = len - got < sizeof buf ? len - got : sizeof buf;
    const size_t n = c.read(buf, want);
    if (!n) break;
    if (!got) {
      char from[16];
      c.peer_ip(from, sizeof from);   // outside any lock: peer_ip() waits on lwIP
      if (!ota_open(len, buf, n, "", "", from, msg, cap)) return false;
    } else if (!ota_write(buf, n, msg, cap)) {
      return false;
    }
    got += n;
  }
  if (got < len) {
    snprintf(msg, cap, "connection ended after %u of %u bytes; nothing changed\n", (unsigned)got, (unsigned)len);
    if (got) ota_abort(msg);
    else Log.printf("ota: %s", msg);
    return false;
  }
  return ota_finish(msg, cap);
}
#endif  // BREWDECK_DEBUG_HTTP
