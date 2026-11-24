// The core's own settings in NVS (ESP-IDF nvs), one namespace per area ("wifi", "machine", "debug"). Same keys and
// types as before (strings, bools as u8), so settings saved by older firmware read back. Each write is committed at
// once. The UI's store (bd_store_*, core.cpp) keeps its own handle.
#pragma once
#include <stdbool.h>
#include <stddef.h>

// copies the string into buf; false (buf = "") when it is missing or does not fit in cap (incl. the terminator)
bool kv_get_str(const char *ns, const char *key, char *buf, size_t cap);
bool kv_get_bool(const char *ns, const char *key, bool def);
void kv_set_str(const char *ns, const char *key, const char *val);
void kv_set_bool(const char *ns, const char *key, bool val);
void kv_erase(const char *ns, const char *key);   // a missing key is fine
