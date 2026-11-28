// Firmware releases over Wi-Fi: checking for, downloading and installing a signed release (see update.h).
#include "update.h"
#include <sdkconfig.h>
#if CONFIG_IDF_TARGET_ESP32S3
// Not on the ESP32-S3 yet: a TLS connection needs ~40 KB of internal RAM and the S3 has ~49 KB free while connected.
// The API answers "not known" and nothing is fetched.
#include "brewdeck_core.h"
const bd_release *bd_release_info(void) { static bd_release r; return &r; }
uint32_t bd_seq_release(void) { return 0; }
void bd_release_check(void) {}
bd_result bd_release_install(void) { return BD_ERR_UNKNOWN_ID; }
const bd_catalog *bd_ui_catalog(void) { static bd_catalog c; return &c; }
bd_result bd_ui_install(const char *) { return BD_ERR_UNKNOWN_ID; }
const bd_picture *bd_ui_preview(const char *) { return NULL; }
void update_begin(const char *) {}
void update_tick(uint32_t) {}
#ifdef BREWDECK_DEBUG_HTTP
void update_set_base(const char *) {}
const char *update_base(void) { return "(not on this board)"; }
#endif
#else
#include <esp_crt_bundle.h>
#include <esp_heap_caps.h>
#include <esp_http_client.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <mbedtls/base64.h>
#include <mbedtls/pk.h>
#include <mbedtls/sha256.h>
#include <stdio.h>
#include <string.h>
#include "brewdeck_core.h"
#include "net.h"
#include "ota.h"
#include "systime.h"
#include "tlog.h"
#include "update_key.h"
#include "update_parse.h"
#ifdef BREWDECK_DEBUG_HTTP
#if __has_include("update_test_key.h")
#include "update_test_key.h"
#define HAVE_TEST_KEY 1
#endif
#endif

static const uint32_t FIRST_CHECK_MS = 90000;            // after the start (and its 60 s trial)
static const uint32_t CHECK_EVERY_MS = 24u * 3600 * 1000, RETRY_MS = 3600u * 1000;
static const char CHIP[] = CONFIG_IDF_TARGET;            // "esp32p4"

static char g_running[40];
static portMUX_TYPE g_mux = portMUX_INITIALIZER_UNLOCKED;
static bd_release g_rel;                                 // under g_mux
static uint32_t g_seq = 1;
static RelCatalog g_cat;                                 // the newest release's signed UI catalogue (under g_mux)
static char g_install_name[16];                          // the UI being installed (bd_ui_install)
// previews (bd_ui_preview): downloaded on request into PSRAM, per catalogue entry; freed only on the UI task
// (update_tick) after the catalogue changed, so a UI copying one in its tick never sees it go
static uint16_t *g_pv_px[REL_MAX_UIS];                   // under g_mux
static uint32_t g_pv_want = 0, g_pv_failed = 0;          // bit per entry; under g_mux
static volatile bool g_pv_reset = false;                 // the catalogue changed: free the old ones
static volatile int g_pv_job = -1;                       // the entry being downloaded
static int cat_find(const char *name);
static volatile bool g_busy = false;                     // a check or download task is running
static volatile bool g_want_check = false, g_want_install = false;
static uint32_t g_next_check = 0;                        // clock_ms of the next scheduled check
#ifdef BREWDECK_DEBUG_HTTP
static char g_base[96] = "";
#endif

static void set_state(bd_release_state st, const char *err) {
  portENTER_CRITICAL(&g_mux);
  g_rel.state = st;
  snprintf(g_rel.error, sizeof g_rel.error, "%s", err ? err : "");
  g_seq++;
  portEXIT_CRITICAL(&g_mux);
}
static void set_installing(bool on, const char *err) {
  portENTER_CRITICAL(&g_mux);
  g_rel.installing = on;
  snprintf(g_rel.error, sizeof g_rel.error, "%s", err ? err : "");
  g_seq++;
  portEXIT_CRITICAL(&g_mux);
}

const bd_release *bd_release_info(void) {
  static bd_release out;
  portENTER_CRITICAL(&g_mux);
  out = g_rel;
  portEXIT_CRITICAL(&g_mux);
  return &out;
}
const bd_picture *bd_ui_preview(const char *name) {
  static bd_picture pic;
  if (!name) return NULL;
  const bd_picture *r = NULL;
  portENTER_CRITICAL(&g_mux);
  const int k = cat_find(name);
  if (k >= 0 && g_pv_px[k] && !g_pv_reset) {
    pic.px = g_pv_px[k];
    pic.w = REL_PREVIEW_W;
    pic.h = REL_PREVIEW_H;
    r = &pic;
  } else if (k >= 0 && g_cat.ui[k].preview[0] && !(g_pv_failed & 1u << k)) {
    g_pv_want |= 1u << k;   // update_tick starts the download
  }
  portEXIT_CRITICAL(&g_mux);
  return r;
}
uint32_t bd_seq_release(void) { return g_seq; }
void bd_release_check(void) { g_want_check = true; }
// the catalogue entry of a UI, -1 = none (call under g_mux)
static int cat_find(const char *name) {
  for (int i = 0; i < g_cat.n; i++)
    if (!strcmp(g_cat.ui[i].name, name)) return i;
  return -1;
}
static bd_result install(const char *name, bool only_newer) {
  char why[48];
  if (ota_machine_busy(why, sizeof why)) return BD_ERR_BUSY;
  portENTER_CRITICAL(&g_mux);
  const bool ok = !g_rel.installing && cat_find(name) >= 0 &&
                  (!only_newer || g_rel.state == BD_RELEASE_AVAILABLE);
  if (ok) snprintf(g_install_name, sizeof g_install_name, "%s", name);
  portEXIT_CRITICAL(&g_mux);
  if (!ok) return BD_ERR_UNKNOWN_ID;
  g_want_install = true;
  set_installing(true, NULL);
  return BD_OK;
}
bd_result bd_release_install(void) { return install(BD_UI.name, true); }
bd_result bd_ui_install(const char *name) { return name ? install(name, false) : BD_ERR_UNKNOWN_ID; }
const bd_catalog *bd_ui_catalog(void) {
  static bd_catalog out;
  portENTER_CRITICAL(&g_mux);
  memset(&out, 0, sizeof out);
  snprintf(out.version, sizeof out.version, "%s", g_cat.version);
  for (int i = 0; i < g_cat.n && i < BD_MAX_UIS; i++) {
    bd_ui_entry *e = &out.ui[out.n++];
    snprintf(e->name, sizeof e->name, "%s", g_cat.ui[i].name);
    snprintf(e->title, sizeof e->title, "%s", g_cat.ui[i].title);
    snprintf(e->about, sizeof e->about, "%s", g_cat.ui[i].about);
    e->size = g_cat.ui[i].size;
    e->current = !strcmp(g_cat.ui[i].name, BD_UI.name);
    e->has_preview = g_cat.ui[i].preview[0] != 0;
  }
  portEXIT_CRITICAL(&g_mux);
  return &out;
}

#ifdef BREWDECK_DEBUG_HTTP
void update_set_base(const char *base) {
  snprintf(g_base, sizeof g_base, "%s", base ? base : "");
  Log.printf("update: releases from %s\n", g_base[0] ? g_base : "GitHub");
  set_state(BD_RELEASE_UNKNOWN, NULL);
  g_want_check = true;
}
const char *update_base(void) { return g_base; }
#endif

// what = "catalog" (the UI catalogue, from v0.3.1) or "update" (Brewdeck's own description, every release)
static void latest_url(const char *what, char *out, size_t cap) {
#ifdef BREWDECK_DEBUG_HTTP
  if (g_base[0]) { snprintf(out, cap, "%s/%s-%s.txt", g_base, what, CHIP); return; }
#endif
  snprintf(out, cap, "https://github.com/" UPDATE_REPO "/releases/latest/download/%s-%s.txt", what, CHIP);
}
static void image_url(const char *ver, const char *file, char *out, size_t cap) {
#ifdef BREWDECK_DEBUG_HTTP
  if (g_base[0]) { snprintf(out, cap, "%s/%s", g_base, file); return; }
#endif
  snprintf(out, cap, "https://github.com/" UPDATE_REPO "/releases/download/%s/%s", ver, file);
}
static const char *source_name(void) {
#ifdef BREWDECK_DEBUG_HTTP
  if (g_base[0]) return "a test server";
#endif
  return "github.com";
}

// ---- HTTP(S) ------------------------------------------------------------------------------------------------------
// Opens url, following redirects (GitHub sends release files to its download host); on success the body can be read
// with esp_http_client_read and *len is its length (-1 = not given). false: err says why, h is cleaned up.
static bool http_open(const char *url, esp_http_client_handle_t *out, int64_t *len, char *err, size_t cap) {
  esp_http_client_config_t cfg = {};
  cfg.url = url;
  cfg.crt_bundle_attach = esp_crt_bundle_attach;   // the usual public certificate authorities (ESP-IDF's bundle)
  cfg.timeout_ms = 15000;
  cfg.buffer_size = 4096;
  cfg.buffer_size_tx = 2048;                       // GitHub's redirect URLs are long (signed query)
  cfg.user_agent = "Brewdeck";
  cfg.keep_alive_enable = false;
  esp_http_client_handle_t h = esp_http_client_init(&cfg);
  if (!h) return snprintf(err, cap, "no memory for the connection"), false;
  for (int hop = 0; hop < 6; hop++) {
    esp_err_t e = esp_http_client_open(h, 0);
    if (e != ESP_OK) {
      snprintf(err, cap, "could not connect (%s)", esp_err_to_name(e));
      break;
    }
    const int64_t cl = esp_http_client_fetch_headers(h);
    const int st = esp_http_client_get_status_code(h);
    if (st == 301 || st == 302 || st == 303 || st == 307 || st == 308) {
      esp_http_client_flush_response(h, NULL);
      if (esp_http_client_set_redirection(h) != ESP_OK) {
        snprintf(err, cap, "bad redirect");
        break;
      }
      continue;
    }
    if (st != 200) {
      snprintf(err, cap, st == 404 ? "not found (HTTP 404)" : "HTTP %d", st);
      break;
    }
    *out = h;
    *len = cl > 0 ? cl : (esp_http_client_is_chunked_response(h) ? -1 : cl);
    return true;
  }
  if (!err[0]) snprintf(err, cap, "too many redirects");
  esp_http_client_cleanup(h);
  return false;
}

// reads up to cap bytes (fewer only at the end); -1 on a read error
static int read_full(esp_http_client_handle_t h, uint8_t *b, int cap) {
  int got = 0;
  while (got < cap) {
    const int n = esp_http_client_read(h, (char *)b + got, cap - got);
    if (n < 0) return -1;
    if (n == 0) break;
    got += n;
  }
  return got;
}

static bool sig_ok(const char *line, size_t ln, const char *sig64, size_t sn, const char *pem) {
  uint8_t der[80], hash[32];
  size_t dl = 0;
  if (mbedtls_base64_decode(der, sizeof der, &dl, (const uint8_t *)sig64, sn) != 0) return false;
  if (mbedtls_sha256((const uint8_t *)line, ln, hash, 0) != 0) return false;
  mbedtls_pk_context pk;
  mbedtls_pk_init(&pk);
  const bool ok = mbedtls_pk_parse_public_key(&pk, (const uint8_t *)pem, strlen(pem) + 1) == 0 &&
                  mbedtls_pk_verify(&pk, MBEDTLS_MD_SHA256, hash, sizeof hash, der, dl) == 0;
  mbedtls_pk_free(&pk);
  return ok;
}

// ---- the two jobs -------------------------------------------------------------------------------------------------
// one signed file of the latest release into text (0-terminated); n = its length, -1 = failed (err says why)
static int fetch_signed(const char *what, char *text, int cap, char *err, size_t ecap, int *status404) {
  char url[160];
  latest_url(what, url, sizeof url);
  esp_http_client_handle_t h;
  int64_t len;
  int n = -1;
  if (http_open(url, &h, &len, err, ecap)) {
    n = read_full(h, (uint8_t *)text, cap - 1);
    esp_http_client_cleanup(h);
    if (n < 0) snprintf(err, ecap, "download broke off");
    else if (n >= cap - 1) { snprintf(err, ecap, "%s file too long", what); n = -1; }
  }
  *status404 = n < 0 && strstr(err, "404") != NULL;
  if (n >= 0) text[n] = 0;
  return n;
}
static bool signed_ok(const char *line, size_t ln, const char *sig, size_t sn) {
  if (sig_ok(line, ln, sig, sn, UPDATE_PUBKEY_PEM)) return true;
#ifdef HAVE_TEST_KEY
  if (g_base[0] && sig_ok(line, ln, sig, sn, UPDATE_TEST_PUBKEY_PEM)) {
    Log.println("update: signed with the TEST key (debug build, test server)");
    return true;
  }
#endif
  return false;
}

static void do_check(void) {
  set_state(BD_RELEASE_CHECKING, NULL);
  char err[48] = "";
  static char text[6144];
  static RelCatalog c;
  int nf;
  // the UI catalogue first; a release from before catalogues has only Brewdeck's own description
  int n = fetch_signed("catalog", text, sizeof text, err, sizeof err, &nf);
  const bool old = n < 0 && nf;
  if (old) n = fetch_signed("update", text, sizeof text, err, sizeof err, &nf);
  const char *bad = NULL;
  if (n < 0) {
    Log.printf("update: check failed: %s\n", err);
    set_state(BD_RELEASE_FAILED, err);
    g_next_check = clock_ms() + RETRY_MS;
    return;
  }
  const char *line, *sig;
  size_t ln, sn;
  if (!rel_split(text, (size_t)n, &line, &ln, &sig, &sn)) bad = "not a release description";
  else if (!signed_ok(line, ln, sig, sn)) bad = "signature does not match";
  else if (old) {
    RelDesc d;
    if (!rel_parse_line(line, ln, &d)) bad = "description unreadable";
    else {   // Brewdeck alone, under its usual file name
      memset(&c, 0, sizeof c);
      snprintf(c.version, sizeof c.version, "%s", d.version);
      snprintf(c.chip, sizeof c.chip, "%s", d.chip);
      c.n = 1;
      snprintf(c.ui[0].name, sizeof c.ui[0].name, "screen");
      snprintf(c.ui[0].title, sizeof c.ui[0].title, "Brewdeck");
      snprintf(c.ui[0].file, sizeof c.ui[0].file, "brewdeck-%s-%s.bin", d.version, CHIP);
      c.ui[0].size = d.size;
      memcpy(c.ui[0].sha256, d.sha256, sizeof d.sha256);
    }
  } else if (!rel_parse_catalog(line, ln, &c)) bad = "catalogue unreadable";
  if (!bad && strcmp(c.chip, CHIP) != 0) bad = "release for another chip";
  if (bad) {
    Log.printf("update: check refused: %s\n", bad);
    set_state(BD_RELEASE_FAILED, bad);
    g_next_check = clock_ms() + RETRY_MS;
    return;
  }
  portENTER_CRITICAL(&g_mux);
  if (strcmp(g_cat.version, c.version) || g_cat.n != c.n) {   // another release: its previews are others
    g_pv_reset = true;
    g_pv_want = g_pv_failed = 0;
  }
  g_cat = c;
  const int me = cat_find(BD_UI.name);
  snprintf(g_rel.version, sizeof g_rel.version, "%s", c.version);
  g_rel.size = me >= 0 ? c.ui[me].size : 0;
  portEXIT_CRITICAL(&g_mux);
  const bool newer = me >= 0 && rel_newer(c.version, g_running);
  set_state(newer ? BD_RELEASE_AVAILABLE : BD_RELEASE_CURRENT,
            me < 0 && rel_newer(c.version, g_running) ? "this interface is not in the newest release" : NULL);
  Log.printf("update: newest release %s (%d interface%s), running %s \"%s\": %s\n", c.version, c.n, c.n == 1 ? "" : "s",
             g_running[0] ? g_running : "?", BD_UI.name,
             newer ? "update available" : me < 0 ? "this interface is not in it" : "up to date");
  g_next_check = clock_ms() + CHECK_EVERY_MS;
}

static void do_install(void) {
  RelDesc d;          // what is checked below: the chosen UI's size and SHA-256 in that release
  char file[sizeof g_cat.ui[0].file], name[sizeof g_install_name], title[sizeof g_cat.ui[0].title];
  portENTER_CRITICAL(&g_mux);
  const int k = cat_find(g_install_name);
  memset(&d, 0, sizeof d);
  if (k >= 0) {
    snprintf(d.version, sizeof d.version, "%s", g_cat.version);
    d.size = g_cat.ui[k].size;
    memcpy(d.sha256, g_cat.ui[k].sha256, sizeof d.sha256);
    snprintf(file, sizeof file, "%s", g_cat.ui[k].file);
    snprintf(title, sizeof title, "%s", g_cat.ui[k].title);
  }
  snprintf(name, sizeof name, "%s", g_install_name);
  portEXIT_CRITICAL(&g_mux);
  if (k < 0) {
    set_installing(false, "not in the catalogue");
    return;
  }
  char url[200], err[48] = "", msg[160] = "";
  image_url(d.version, file, url, sizeof url);
  Log.printf("update: downloading %s for \"%s\"%s\n", d.version, name, strcmp(name, BD_UI.name) ? " (another interface)" : "");
  esp_http_client_handle_t h;
  int64_t len;
  if (!http_open(url, &h, &len, err, sizeof err)) {
    Log.printf("update: download failed: %s\n", err);
    set_installing(false, err);
    return;
  }
  bool ok = false;
  static uint8_t buf[4096];
  mbedtls_sha256_context sha;
  mbedtls_sha256_init(&sha);
  mbedtls_sha256_starts(&sha, 0);
  uint32_t got = 0;
  const uint32_t t0 = clock_ms();
  if (len >= 0 && len != (int64_t)d.size) {
    snprintf(err, sizeof err, "size %lld, expected %u", (long long)len, (unsigned)d.size);
  } else {
    bool opened = false;
    while (got < d.size) {
      const int want = d.size - got < sizeof buf ? (int)(d.size - got) : (int)sizeof buf;
      const int n = read_full(h, buf, want);
      if (n <= 0) {
        snprintf(err, sizeof err, n < 0 ? "download broke off" : "download ended early");
        break;
      }
      char why[48];
      if (opened && ota_machine_busy(why, sizeof why)) {   // a shot, steam, hot water or flush started: stop
        snprintf(err, sizeof err, "stopped: %s", why + 9);   // "refused: a shot is running\n" -> "a shot is running"
        err[strcspn(err, "\n")] = 0;
        break;
      }
      mbedtls_sha256_update(&sha, buf, (size_t)n);
      if (!opened ? ota_open(d.size, buf, (size_t)n, d.version, title, source_name(), msg, sizeof msg)
                  : ota_write(buf, (size_t)n, msg, sizeof msg)) {
        opened = true;
      } else {
        snprintf(err, sizeof err, "%.*s", (int)strcspn(msg, "\n"), msg);
        opened = false;   // ota_open / ota_write cleaned up
        break;
      }
      got += (uint32_t)n;
    }
    uint8_t sum[32];
    mbedtls_sha256_finish(&sha, sum);
    if (!err[0] && memcmp(sum, d.sha256, sizeof sum) != 0) snprintf(err, sizeof err, "checksum does not match");
    if (err[0]) {
      if (opened) ota_abort(err);
    } else if (ota_finish(msg, sizeof msg)) {
      ok = true;
    } else {
      snprintf(err, sizeof err, "%.*s", (int)strcspn(msg, "\n"), msg);
    }
  }
  mbedtls_sha256_free(&sha);
  esp_http_client_cleanup(h);
  if (!ok) {
    Log.printf("update: %s not installed: %s\n", d.version, err);
    set_installing(false, err);
    return;
  }
  Log.printf("update: %s downloaded and checked in %.1f s\n", d.version, (clock_ms() - t0) / 1000.0f);
  ota_restart();
}

// one preview into PSRAM, checked against its signed SHA-256
static void do_preview(int k) {
  char url[200], err[48] = "", file[sizeof g_cat.ui[0].preview], ver[sizeof g_cat.version];
  uint8_t want[32];
  uint32_t size;
  portENTER_CRITICAL(&g_mux);
  snprintf(file, sizeof file, "%s", g_cat.ui[k].preview);
  snprintf(ver, sizeof ver, "%s", g_cat.version);
  size = g_cat.ui[k].psize;
  memcpy(want, g_cat.ui[k].psha256, sizeof want);
  portEXIT_CRITICAL(&g_mux);
  image_url(ver, file, url, sizeof url);
  uint16_t *px = (uint16_t *)heap_caps_malloc(size, MALLOC_CAP_SPIRAM);
  esp_http_client_handle_t h;
  int64_t len;
  bool ok = false;
  if (px && http_open(url, &h, &len, err, sizeof err)) {
    const int n = read_full(h, (uint8_t *)px, (int)size);
    esp_http_client_cleanup(h);
    uint8_t sum[32];
    ok = n == (int)size && mbedtls_sha256((const uint8_t *)px, size, sum, 0) == 0 && !memcmp(sum, want, sizeof sum);
    if (!ok && !err[0]) snprintf(err, sizeof err, n == (int)size ? "checksum does not match" : "incomplete");
  } else if (!px) {
    snprintf(err, sizeof err, "no memory");
  }
  portENTER_CRITICAL(&g_mux);
  const bool same = !strcmp(ver, g_cat.version) && !g_pv_reset;
  if (ok && same && !g_pv_px[k]) { g_pv_px[k] = px; px = NULL; }
  else if (!ok) g_pv_failed |= 1u << k;
  g_seq++;
  portEXIT_CRITICAL(&g_mux);
  if (px) heap_caps_free(px);
  if (!ok) Log.printf("update: preview %s: %s\n", file, err);
}

static void job(void *arg) {
  if (g_pv_job >= 0) { do_preview(g_pv_job); g_pv_job = -1; }
  else if (arg) do_install();
  else do_check();
  g_busy = false;
  vTaskDelete(NULL);
}

static void start(bool install) {
  g_busy = true;
  // the stack must be in internal RAM: flash is written from this task
  if (xTaskCreatePinnedToCore(job, install ? "update_dl" : "update_chk", 8192, install ? (void *)1 : NULL, 2, NULL, 0)
      != pdPASS) {
    g_busy = false;
    g_pv_job = -1;
    Log.println("update: no memory for the task");
    if (install) set_installing(false, "no memory");
  }
}

void update_begin(const char *running_version) {
  snprintf(g_running, sizeof g_running, "%s", running_version ? running_version : "");
  g_next_check = clock_ms() + FIRST_CHECK_MS;
}

void update_tick(uint32_t now) {
  if (g_pv_reset && !g_busy) {   // on the UI task: no UI tick is copying a preview now
    portENTER_CRITICAL(&g_mux);
    uint16_t *old[REL_MAX_UIS];
    memcpy(old, g_pv_px, sizeof old);
    memset(g_pv_px, 0, sizeof g_pv_px);
    g_pv_reset = false;
    portEXIT_CRITICAL(&g_mux);
    for (int k = 0; k < REL_MAX_UIS; k++) if (old[k]) heap_caps_free(old[k]);
  }
  if (g_busy) return;
  if (g_want_install) {
    g_want_install = false;
    start(true);
    return;
  }
  portENTER_CRITICAL(&g_mux);
  int pk = -1;
  for (int k = 0; k < REL_MAX_UIS && pk < 0; k++)
    if (g_pv_want & 1u << k) { pk = k; g_pv_want &= ~(1u << k); }
  portEXIT_CRITICAL(&g_mux);
  if (pk >= 0 && net_wifi_up(NULL)) {   // a preview the UI shows now
    g_pv_job = pk;
    start(false);
    return;
  }
  const bool due = g_want_check || (int32_t)(now - g_next_check) >= 0;
  if (!due || !net_wifi_up(NULL) || ota_on_trial()) return;
  char why[48];
  if (ota_machine_busy(why, sizeof why)) return;   // not while the machine is busy; tried again on a later pass
  g_want_check = false;
  start(false);
}
#endif  // not CONFIG_IDF_TARGET_ESP32S3
