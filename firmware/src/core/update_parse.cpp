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

// the number of "key":123 in line (1 .. 4 GB); false = missing, malformed or 0
static bool num_field(const char *line, size_t n, const char *key, uint32_t *out) {
  char pat[24];
  const size_t pn = (size_t)snprintf(pat, sizeof pat, "\"%s\":", key);
  for (size_t i = 0; i + pn < n; i++) {
    if (memcmp(line + i, pat, pn) != 0) continue;
    size_t j = i + pn;
    if (line[j] < '0' || line[j] > '9') return false;
    uint64_t s = 0;
    while (j < n && line[j] >= '0' && line[j] <= '9') {
      s = s * 10 + (uint64_t)(line[j++] - '0');
      if (s > 0xFFFFFFFFu) return false;
    }
    *out = (uint32_t)s;
    return s > 0;
  }
  return false;
}

static int hexval(char c) {
  return c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : c >= 'A' && c <= 'F' ? c - 'A' + 10 : -1;
}
static bool sha_field(const char *line, size_t n, uint8_t out[32], const char *key = "sha256") {
  char hex[66];
  if (!str_field(line, n, key, hex, sizeof hex) || strlen(hex) != 64) return false;
  for (int i = 0; i < 32; i++) {
    const int a = hexval(hex[2 * i]), b = hexval(hex[2 * i + 1]);
    if (a < 0 || b < 0) return false;
    out[i] = (uint8_t)(a << 4 | b);
  }
  return true;
}
static bool plain_version(const char *v) {
  unsigned x[3];
  size_t used;
  return read_ver(v, strlen(v), x, &used) && !v[used];
}

bool rel_parse_line(const char *line, size_t n, RelDesc *out) {
  memset(out, 0, sizeof *out);
  return str_field(line, n, "version", out->version, sizeof out->version) && plain_version(out->version) &&
         str_field(line, n, "chip", out->chip, sizeof out->chip) && out->chip[0] && sha_field(line, n, out->sha256) &&
         num_field(line, n, "size", &out->size);
}

static bool name_ok(const char *s) {   // a BD_UI name: lower case letters, digits, '-'
  if (!s[0]) return false;
  for (; *s; s++)
    if (!((*s >= 'a' && *s <= 'z') || (*s >= '0' && *s <= '9') || *s == '-')) return false;
  return true;
}
static bool file_ok(const char *s) {   // a plain file name, never a path
  if (!s[0] || s[0] == '.') return false;
  for (; *s; s++)
    if (!((*s >= 'a' && *s <= 'z') || (*s >= 'A' && *s <= 'Z') || (*s >= '0' && *s <= '9') || *s == '.' ||
          *s == '_' || *s == '-')) return false;
  return true;
}

bool rel_parse_catalog(const char *line, size_t n, RelCatalog *out) {
  memset(out, 0, sizeof *out);
  static const char key[] = "\"uis\":[";
  const char *arr = NULL;
  for (size_t i = 0; i + sizeof key - 1 <= n; i++)
    if (!memcmp(line + i, key, sizeof key - 1)) { arr = line + i + sizeof key - 1; break; }
  if (!arr) return false;
  const size_t head = (size_t)(arr - line);   // version and chip: before the list
  if (!str_field(line, head, "version", out->version, sizeof out->version) || !plain_version(out->version) ||
      !str_field(line, head, "chip", out->chip, sizeof out->chip) || !out->chip[0]) return false;
  const char *p = arr, *end = line + n;
  while (p < end && *p != ']') {
    const char *o = (const char *)memchr(p, '{', (size_t)(end - p));
    if (!o) return false;
    const char *c = (const char *)memchr(o, '}', (size_t)(end - o));
    if (!c) return false;
    if (out->n < REL_MAX_UIS) {
      RelUi *u = &out->ui[out->n];
      const size_t on = (size_t)(c - o);
      if (!str_field(o, on, "name", u->name, sizeof u->name) || !name_ok(u->name) ||
          !str_field(o, on, "title", u->title, sizeof u->title) || !u->title[0] ||
          !str_field(o, on, "file", u->file, sizeof u->file) || !file_ok(u->file) ||
          !num_field(o, on, "size", &u->size) || !sha_field(o, on, u->sha256)) return false;
      if (!str_field(o, on, "about", u->about, sizeof u->about)) u->about[0] = 0;   // optional
      if (str_field(o, on, "preview", u->preview, sizeof u->preview)) {            // optional, all three or none
        if (!file_ok(u->preview) || !num_field(o, on, "psize", &u->psize) ||
            u->psize % (REL_PREVIEW_W * REL_PREVIEW_H * 2) ||
            u->psize / (REL_PREVIEW_W * REL_PREVIEW_H * 2) > REL_PREVIEW_FRAMES ||
            !sha_field(o, on, u->psha256, "psha256")) return false;
      } else u->preview[0] = 0;
      for (int k = 0; k < out->n; k++)
        if (!strcmp(out->ui[k].name, u->name)) return false;                     // a name twice
      out->n++;
    }
    p = c + 1;
    while (p < end && (*p == ',' || *p == ' ')) p++;
  }
  return out->n > 0;
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
