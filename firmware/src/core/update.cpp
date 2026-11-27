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
void update_begin(const char *) {}
void update_tick(uint32_t) {}
#ifdef BREWDECK_DEBUG_HTTP
void update_set_base(const char *) {}
const char *update_base(void) { return "(not on this board)"; }
#endif
#else
#include <esp_crt_bundle.h>
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
static RelDesc g_desc;                                   // the newest release's signed description (under g_mux)
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
uint32_t bd_seq_release(void) { return g_seq; }
void bd_release_check(void) { g_want_check = true; }
bd_result bd_release_install(void) {
  char why[48];
  if (ota_machine_busy(why, sizeof why)) return BD_ERR_BUSY;
  portENTER_CRITICAL(&g_mux);
  const bool avail = g_rel.state == BD_RELEASE_AVAILABLE && !g_rel.installing;
  portEXIT_CRITICAL(&g_mux);
  if (!avail) return BD_ERR_UNKNOWN_ID;
  g_want_install = true;
  set_installing(true, NULL);
  return BD_OK;
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

static void manifest_url(char *out, size_t cap) {
#ifdef BREWDECK_DEBUG_HTTP
  if (g_base[0]) { snprintf(out, cap, "%s/update-%s.txt", g_base, CHIP); return; }
#endif
  snprintf(out, cap, "https://github.com/" UPDATE_REPO "/releases/latest/download/update-%s.txt", CHIP);
}
static void image_url(const char *ver, char *out, size_t cap) {
#ifdef BREWDECK_DEBUG_HTTP
  if (g_base[0]) { snprintf(out, cap, "%s/brewdeck-%s-%s.bin", g_base, ver, CHIP); return; }
#endif
  snprintf(out, cap, "https://github.com/" UPDATE_REPO "/releases/download/%s/brewdeck-%s-%s.bin", ver, ver, CHIP);
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
static void do_check(void) {
  set_state(BD_RELEASE_CHECKING, NULL);
  char url[160], err[48] = "";
  manifest_url(url, sizeof url);
  esp_http_client_handle_t h;
  int64_t len;
  static char text[1024];
  int n = -1;
  if (http_open(url, &h, &len, err, sizeof err)) {
    n = read_full(h, (uint8_t *)text, sizeof text - 1);
    esp_http_client_cleanup(h);
    if (n < 0) snprintf(err, sizeof err, "download broke off");
  }
  if (n < 0) {
    Log.printf("update: check failed: %s\n", err);
    set_state(BD_RELEASE_FAILED, err);
    g_next_check = clock_ms() + RETRY_MS;
    return;
  }
  text[n] = 0;
  const char *line, *sig;
  size_t ln, sn;
  RelDesc d;
  const char *bad = !rel_split(text, (size_t)n, &line, &ln, &sig, &sn) ? "not a release description" : NULL;
  if (!bad) {
    bool ok = sig_ok(line, ln, sig, sn, UPDATE_PUBKEY_PEM);
#ifdef HAVE_TEST_KEY
    if (!ok && g_base[0] && sig_ok(line, ln, sig, sn, UPDATE_TEST_PUBKEY_PEM)) {
      ok = true;
      Log.println("update: signed with the TEST key (debug build, test server)");
    }
#endif
    bad = !ok ? "signature does not match" : !rel_parse_line(line, ln, &d) ? "description unreadable"
        : strcmp(d.chip, CHIP) != 0 ? "release for another chip" : NULL;
  }
  if (bad) {
    Log.printf("update: check refused: %s\n", bad);
    set_state(BD_RELEASE_FAILED, bad);
    g_next_check = clock_ms() + RETRY_MS;
    return;
  }
  const bool newer = rel_newer(d.version, g_running);
  portENTER_CRITICAL(&g_mux);
  g_desc = d;
  snprintf(g_rel.version, sizeof g_rel.version, "%s", d.version);
  g_rel.size = d.size;
  portEXIT_CRITICAL(&g_mux);
  set_state(newer ? BD_RELEASE_AVAILABLE : BD_RELEASE_CURRENT, NULL);
  Log.printf("update: newest release %s (%u bytes), running %s: %s\n", d.version, (unsigned)d.size,
             g_running[0] ? g_running : "?", newer ? "update available" : "up to date");
  g_next_check = clock_ms() + CHECK_EVERY_MS;
}

static void do_install(void) {
  RelDesc d;
  portENTER_CRITICAL(&g_mux);
  d = g_desc;
  portEXIT_CRITICAL(&g_mux);
  char url[200], err[48] = "", msg[160] = "";
  image_url(d.version, url, sizeof url);
  Log.printf("update: downloading %s\n", d.version);
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
      if (!opened ? ota_open(d.size, buf, (size_t)n, d.version, source_name(), msg, sizeof msg)
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

static void job(void *arg) {
  if (arg) do_install();
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
    Log.println("update: no memory for the task");
    if (install) set_installing(false, "no memory");
  }
}

void update_begin(const char *running_version) {
  snprintf(g_running, sizeof g_running, "%s", running_version ? running_version : "");
  g_next_check = clock_ms() + FIRST_CHECK_MS;
}

void update_tick(uint32_t now) {
  if (g_busy) return;
  if (g_want_install) {
    g_want_install = false;
    start(true);
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
