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
// The UI catalogue of a release, catalog-<chip>.txt (tools/uis.py catalog), signed the same way:
//   line 1: {"version":"v0.3.1","chip":"esp32p4","uis":[{"name":"screen","title":"Brewdeck","file":"brewdeck-...bin",
//            "size":1503232,"sha256":"<64 hex>","about":"...",
//            "preview":"preview-v0.3.1-screen.rgb565","psize":122880,"psha256":"<64 hex>"}, ...]}
// preview (optional): the interface's Ready screen, 320 x 192 RGB565 little-endian (REL_PREVIEW_W / _H).
// Titles and descriptions never contain { } [ ] " or \ (uis.py refuses them), so each entry is the text between one
// { and the next }.
#define REL_MAX_UIS 8
#define REL_PREVIEW_W 320
#define REL_PREVIEW_H 192
typedef struct {
  char     name[16];     // the UI's BD_UI name ("screen" = Brewdeck's own)
  char     title[24];
  char     file[64];     // the image's file name in the release (letters, digits, . _ - only)
  char     about[96];
  uint32_t size;
  uint8_t  sha256[32];
  char     preview[64];  // "" = none
  uint32_t psize;        // REL_PREVIEW_W * REL_PREVIEW_H * 2 when there is one
  uint8_t  psha256[32];
} RelUi;
typedef struct {
  char  version[16];
  char  chip[12];
  int   n;
  RelUi ui[REL_MAX_UIS];
} RelCatalog;
// Reads line 1 of a catalogue. false = malformed (no entry, a field missing, a bad name or file name); entries past
// REL_MAX_UIS are ignored.
bool rel_parse_catalog(const char *line, size_t n, RelCatalog *out);

// true when release (vX.Y.Z) is newer than running ("v0.2.0", "v0.2.0-57-g53a4a5a-dirty" = after v0.2.0).
// A running version without a vX.Y.Z start (not known) is never offered an update.
bool rel_newer(const char *release, const char *running);
