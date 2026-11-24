// The core's settings in NVS (kv.h).
#include "kv.h"
#include <nvs.h>
#include "tlog.h"

bool kv_get_str(const char *ns, const char *key, char *buf, size_t cap) {
  if (cap) buf[0] = 0;
  nvs_handle_t h;
  if (nvs_open(ns, NVS_READONLY, &h) != ESP_OK) return false;   // namespace never written
  size_t n = cap;
  const esp_err_t e = nvs_get_str(h, key, buf, &n);                // ESP_ERR_NVS_INVALID_LENGTH: does not fit
  nvs_close(h);
  if (e != ESP_OK && cap) buf[0] = 0;
  return e == ESP_OK;
}

bool kv_get_bool(const char *ns, const char *key, bool def) {
  nvs_handle_t h;
  if (nvs_open(ns, NVS_READONLY, &h) != ESP_OK) return def;
  uint8_t v = 0;
  const esp_err_t e = nvs_get_u8(h, key, &v);
  nvs_close(h);
  return e == ESP_OK ? v != 0 : def;
}

// opens the namespace for writing, runs the change, commits; logs a failure (never the value: it may be a password)
static void write(const char *ns, const char *key, esp_err_t (*op)(nvs_handle_t, const char *, const void *),
                  const void *arg) {
  nvs_handle_t h;
  esp_err_t e = nvs_open(ns, NVS_READWRITE, &h);
  if (e == ESP_OK) {
    e = op(h, key, arg);
    if (e == ESP_ERR_NVS_NOT_FOUND) e = ESP_OK;   // erasing a key that is not there
    if (e == ESP_OK) e = nvs_commit(h);
    nvs_close(h);
  }
  if (e != ESP_OK) Log.printf("kv: %s/%s not saved (%s)\n", ns, key, esp_err_to_name(e));
}
static esp_err_t op_str(nvs_handle_t h, const char *k, const void *a) { return nvs_set_str(h, k, (const char *)a); }
static esp_err_t op_u8(nvs_handle_t h, const char *k, const void *a) { return nvs_set_u8(h, k, *(const uint8_t *)a); }
static esp_err_t op_erase(nvs_handle_t h, const char *k, const void *) { return nvs_erase_key(h, k); }

void kv_set_str(const char *ns, const char *key, const char *val) { write(ns, key, op_str, val ? val : ""); }
void kv_set_bool(const char *ns, const char *key, bool val) {
  const uint8_t v = val ? 1 : 0;
  write(ns, key, op_u8, &v);
}
void kv_erase(const char *ns, const char *key) { write(ns, key, op_erase, nullptr); }
