// Pure helpers for firmware releases (see update_parse.h).
#include "update_parse.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

bool rel_split(const char *text, size_t n, const char **line, size_t *line_n, const char **sig, size_t *sig_n) {
  const char *nl = (const char *)memchr(text, '\n', n);
  if (!nl || nl == text) return false;
  *line = text;
  *line_n = (size_t)(nl - text);
  const char *s = nl + 1, *end = text + n;
  size_t k = 0;
  while (s + k < end && s[k] != '\n' && s[k] != '\r') k++;
  if (!k) return false;
  *sig = s;
  *sig_n = k;
  return true;
}

// "v1.2.3" at the start of s: the three numbers and the length read; false otherwise
static bool read_ver(const char *s, size_t n, unsigned v[3], size_t *used) {
  if (n < 6 || s[0] != 'v') return false;
  size_t i = 1;
  for (int part = 0; part < 3; part++) {
    if (i >= n || s[i] < '0' || s[i] > '9') return false;
    unsigned x = 0;
    while (i < n && s[i] >= '0' && s[i] <= '9') {
      if (x > 99999) return false;
      x = x * 10 + (unsigned)(s[i++] - '0');
    }
    v[part] = x;
    if (part < 2) {
      if (i >= n || s[i] != '.') return false;
      i++;
    }
  }
  *used = i;
  return true;
}

// the text value of "key":"..." in line (at most cap - 1 characters); false = missing or too long
static bool str_field(const char *line, size_t n, const char *key, char *out, size_t cap) {
  char pat[24];
  const size_t pn = (size_t)snprintf(pat, sizeof pat, "\"%s\":\"", key);
  for (size_t i = 0; i + pn <= n; i++) {
    if (memcmp(line + i, pat, pn) != 0) continue;
    size_t j = i + pn, k = 0;
    while (j < n && line[j] != '"') {
      if (k + 1 >= cap) return false;
      out[k++] = line[j++];
    }
    if (j >= n) return false;
    out[k] = 0;
    return true;
  }
  return false;
}

static int hexval(char c) {
  return c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : c >= 'A' && c <= 'F' ? c - 'A' + 10 : -1;
}

bool rel_parse_line(const char *line, size_t n, RelDesc *out) {
  memset(out, 0, sizeof *out);
  if (!str_field(line, n, "version", out->version, sizeof out->version)) return false;
  unsigned v[3];
  size_t used;
  if (!read_ver(out->version, strlen(out->version), v, &used) || out->version[used]) return false;
  if (!str_field(line, n, "chip", out->chip, sizeof out->chip) || !out->chip[0]) return false;
  char hex[66];
  if (!str_field(line, n, "sha256", hex, sizeof hex) || strlen(hex) != 64) return false;
  for (int i = 0; i < 32; i++) {
    const int a = hexval(hex[2 * i]), b = hexval(hex[2 * i + 1]);
    if (a < 0 || b < 0) return false;
    out->sha256[i] = (uint8_t)(a << 4 | b);
  }
  static const char key[] = "\"size\":";
  const size_t kn = sizeof key - 1;
  for (size_t i = 0; i + kn < n; i++) {
    if (memcmp(line + i, key, kn) != 0) continue;
    size_t j = i + kn;
    if (line[j] < '0' || line[j] > '9') return false;
    uint64_t s = 0;
    while (j < n && line[j] >= '0' && line[j] <= '9') {
      s = s * 10 + (uint64_t)(line[j++] - '0');
      if (s > 0xFFFFFFFFu) return false;
    }
    out->size = (uint32_t)s;
    return s > 0;
  }
  return false;
}

bool rel_newer(const char *release, const char *running) {
  unsigned a[3], b[3];
  size_t ua, ub;
  if (!release || !running || !read_ver(release, strlen(release), a, &ua) || release[ua]) return false;
  if (!read_ver(running, strlen(running), b, &ub)) return false;
  for (int i = 0; i < 3; i++)
    if (a[i] != b[i]) return a[i] > b[i];
  return false;   // the same release, or a build after it ("v0.2.0-57-g...")
}
