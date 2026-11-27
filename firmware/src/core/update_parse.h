#pragma once
// Pure helpers for firmware releases (update.cpp), host-tested: the signed description update-<chip>.txt
// (tools/sign-update.sh) and version comparison.
//   line 1: {"version":"v0.3.0","chip":"esp32p4","size":1503232,"sha256":"<64 hex>"}
//   line 2: base64 of the DER ECDSA P-256 / SHA-256 signature of line 1 (without its newline)
#include <stddef.h>
#include <stdint.h>

typedef struct {
  char     version[16];
  char     chip[12];
  uint32_t size;
  uint8_t  sha256[32];
} RelDesc;

// Splits the file into the signed line and the signature text (both point into text). false = not two lines.
bool rel_split(const char *text, size_t n, const char **line, size_t *line_n, const char **sig, size_t *sig_n);
// Reads line 1. false = a field missing or malformed (version must be vX.Y.Z, sha256 64 hex digits, size > 0).
bool rel_parse_line(const char *line, size_t n, RelDesc *out);
// true when release (vX.Y.Z) is newer than running ("v0.2.0", "v0.2.0-57-g53a4a5a-dirty" = after v0.2.0).
// A running version without a vX.Y.Z start (not known) is never offered an update.
bool rel_newer(const char *release, const char *running);
