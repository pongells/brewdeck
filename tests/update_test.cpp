// Firmware release description + version comparison (firmware/src/core/update_parse.cpp). Run via tests/run.sh.
// The signature itself is checked on the device (mbedTLS); tools/sign-update.sh checks what it writes.
#include <string.h>
#include "test_util.h"
#include "update_parse.h"

static const char GOOD[] =
    "{\"version\":\"v0.3.0\",\"chip\":\"esp32p4\",\"size\":1503232,"
    "\"sha256\":\"f2a8afcf013958cfd3c113bccd374332ef5a42641f6c520a6b1c5418d4f928e6\"}\n"
    "MEUCIQCTt6UV42Pw5Iwdw8Sy1mhQGdtp24wJhVI6YjkH6EKEZgIgTpkLjoDe8tK2ZvSFEKlBIGlVYt7UlQCU/KDk2q6c1w0=\n";

static bool parse_text(const char *t, RelDesc *d) {
  const char *line, *sig;
  size_t ln, sn;
  return rel_split(t, strlen(t), &line, &ln, &sig, &sn) && rel_parse_line(line, ln, d);
}

int main() {
  // the file as tools/sign-update.sh writes it
  const char *line, *sig;
  size_t ln, sn;
  CHECK(rel_split(GOOD, strlen(GOOD), &line, &ln, &sig, &sn));
  CHECK(ln == (size_t)(strchr(GOOD, '\n') - GOOD) && line[ln - 1] == '}');   // the signed bytes: line 1, no newline
  CHECK(sn == 96 && sig[0] == 'M' && sig[sn - 1] == '=');
  RelDesc d;
  CHECK(rel_parse_line(line, ln, &d));
  CHECK(!strcmp(d.version, "v0.3.0") && !strcmp(d.chip, "esp32p4") && d.size == 1503232);
  CHECK(d.sha256[0] == 0xf2 && d.sha256[1] == 0xa8 && d.sha256[31] == 0xe6);
  // a CRLF file and one without the last newline still split
  CHECK(parse_text("{\"version\":\"v1.0.0\",\"chip\":\"esp32s3\",\"size\":5,\"sha256\":\""
                   "00112233445566778899aabbccddeeff00112233445566778899AABBCCDDEEFF\"}\nAAAA\r\n", &d));
  CHECK(d.sha256[16] == 0x00 && d.sha256[31] == 0xff && d.size == 5);
  // malformed: refused
  CHECK(!parse_text("{\"version\":\"v0.3.0\"}", &d));                                     // one line only
  CHECK(!parse_text("{\"version\":\"v0.3.0\",\"chip\":\"esp32p4\",\"size\":1}\nAAAA\n", &d));   // no sha256
  CHECK(!parse_text("{\"version\":\"0.3.0\",\"chip\":\"esp32p4\",\"size\":1,\"sha256\":\""
                    "f2a8afcf013958cfd3c113bccd374332ef5a42641f6c520a6b1c5418d4f928e6\"}\nAAAA\n", &d));   // no v
  CHECK(!parse_text("{\"version\":\"v0.3.0-rc1\",\"chip\":\"esp32p4\",\"size\":1,\"sha256\":\""
                    "f2a8afcf013958cfd3c113bccd374332ef5a42641f6c520a6b1c5418d4f928e6\"}\nAAAA\n", &d));   // not plain
  CHECK(!parse_text("{\"version\":\"v0.3.0\",\"chip\":\"esp32p4\",\"size\":0,\"sha256\":\""
                    "f2a8afcf013958cfd3c113bccd374332ef5a42641f6c520a6b1c5418d4f928e6\"}\nAAAA\n", &d));   // size 0
  CHECK(!parse_text("{\"version\":\"v0.3.0\",\"chip\":\"esp32p4\",\"size\":99999999999,\"sha256\":\""
                    "f2a8afcf013958cfd3c113bccd374332ef5a42641f6c520a6b1c5418d4f928e6\"}\nAAAA\n", &d));   // > 4 GB
  CHECK(!parse_text("{\"version\":\"v0.3.0\",\"chip\":\"esp32p4\",\"size\":1,\"sha256\":\""
                    "g2a8afcf013958cfd3c113bccd374332ef5a42641f6c520a6b1c5418d4f928e6\"}\nAAAA\n", &d));   // not hex
  CHECK(!parse_text("{\"version\":\"v0.3.0\",\"chip\":\"esp32p4\",\"size\":1,\"sha256\":\"f2a8\"}\nAAAA\n", &d));
  CHECK(!parse_text("{\"version\":\"v123456789.0.0\",\"chip\":\"esp32p4\",\"size\":1,\"sha256\":\""
                    "f2a8afcf013958cfd3c113bccd374332ef5a42641f6c520a6b1c5418d4f928e6\"}\nAAAA\n", &d));   // too long
  CHECK(!parse_text("\nAAAA\n", &d));
  CHECK(!parse_text("{\"version\":\"v0.3.0\"}\n\n", &d));                                  // empty signature

  // the UI catalogue (tools/uis.py catalog)
  {
    const char cat[] =
        "{\"version\":\"v0.3.1\",\"chip\":\"esp32p4\",\"uis\":["
        "{\"name\":\"screen\",\"title\":\"Brewdeck\",\"file\":\"brewdeck-v0.3.1-esp32p4.bin\",\"size\":1430000,"
        "\"sha256\":\"f2a8afcf013958cfd3c113bccd374332ef5a42641f6c520a6b1c5418d4f928e6\",\"about\":\"This screen's own.\"},"
        "{\"name\":\"ledger\",\"title\":\"Ledger\",\"file\":\"brewdeck-v0.3.1-ledger-esp32p4.bin\",\"size\":1420000,"
        "\"sha256\":\"00112233445566778899aabbccddeeff00112233445566778899aabbccddeeff\"}]}";
    RelCatalog c;
    CHECK(rel_parse_catalog(cat, strlen(cat), &c));
    CHECK(c.n == 2 && !strcmp(c.version, "v0.3.1") && !strcmp(c.chip, "esp32p4"));
    CHECK(!strcmp(c.ui[0].name, "screen") && !strcmp(c.ui[0].title, "Brewdeck") && c.ui[0].size == 1430000);
    CHECK(!strcmp(c.ui[0].file, "brewdeck-v0.3.1-esp32p4.bin") && !strcmp(c.ui[0].about, "This screen's own."));
    CHECK(!strcmp(c.ui[1].name, "ledger") && c.ui[1].about[0] == 0 && c.ui[1].sha256[31] == 0xff);
    CHECK(c.ui[0].preview[0] == 0 && c.ui[1].preview[0] == 0);   // previews are optional
    const char withp[] =
        "{\"version\":\"v0.3.1\",\"chip\":\"esp32p4\",\"uis\":[{\"name\":\"screen\",\"title\":\"Brewdeck\","
        "\"file\":\"b.bin\",\"size\":9,\"sha256\":\"f2a8afcf013958cfd3c113bccd374332ef5a42641f6c520a6b1c5418d4f928e6\","
        "\"preview\":\"preview-v0.3.1-screen.rgb565\",\"psize\":122880,"
        "\"psha256\":\"00112233445566778899aabbccddeeff00112233445566778899aabbccddeeff\"}]}";
    CHECK(rel_parse_catalog(withp, strlen(withp), &c));
    CHECK(!strcmp(c.ui[0].preview, "preview-v0.3.1-screen.rgb565") && c.ui[0].psize == 122880);
    CHECK(c.ui[0].psha256[0] == 0x00 && c.ui[0].psha256[31] == 0xff && c.ui[0].sha256[0] == 0xf2);   // not mixed up
    char badp[sizeof withp];
    memcpy(badp, withp, sizeof withp);
    memcpy(strstr(badp, "122880"), "122881", 6);                 // a preview of the wrong size: refused
    CHECK(!rel_parse_catalog(badp, strlen(badp), &c));
    // refused: a path as the file name, an upper-case name, a name twice, no entries, a bad hash
    struct { const char *what, *text; } bad[] = {
        {"path", "{\"version\":\"v0.3.1\",\"chip\":\"esp32p4\",\"uis\":[{\"name\":\"x\",\"title\":\"X\",\"file\":\"../x.bin\","
                 "\"size\":1,\"sha256\":\"f2a8afcf013958cfd3c113bccd374332ef5a42641f6c520a6b1c5418d4f928e6\"}]}"},
        {"name", "{\"version\":\"v0.3.1\",\"chip\":\"esp32p4\",\"uis\":[{\"name\":\"X\",\"title\":\"X\",\"file\":\"x.bin\","
                 "\"size\":1,\"sha256\":\"f2a8afcf013958cfd3c113bccd374332ef5a42641f6c520a6b1c5418d4f928e6\"}]}"},
        {"twice", "{\"version\":\"v0.3.1\",\"chip\":\"esp32p4\",\"uis\":["
                  "{\"name\":\"x\",\"title\":\"X\",\"file\":\"x.bin\",\"size\":1,"
                  "\"sha256\":\"f2a8afcf013958cfd3c113bccd374332ef5a42641f6c520a6b1c5418d4f928e6\"},"
                  "{\"name\":\"x\",\"title\":\"Y\",\"file\":\"y.bin\",\"size\":1,"
                  "\"sha256\":\"f2a8afcf013958cfd3c113bccd374332ef5a42641f6c520a6b1c5418d4f928e6\"}]}"},
        {"empty", "{\"version\":\"v0.3.1\",\"chip\":\"esp32p4\",\"uis\":[]}"},
        {"hash", "{\"version\":\"v0.3.1\",\"chip\":\"esp32p4\",\"uis\":[{\"name\":\"x\",\"title\":\"X\",\"file\":\"x.bin\","
                 "\"size\":1,\"sha256\":\"f2a8\"}]}"},
        {"no list", "{\"version\":\"v0.3.1\",\"chip\":\"esp32p4\"}"},
    };
    for (auto &b : bad) {
      const bool ok = rel_parse_catalog(b.text, strlen(b.text), &c);
      if (ok) printf("FAIL catalogue accepted: %s\n", b.what);
      CHECK(!ok);
    }
  }

  // versions
  CHECK(rel_newer("v0.3.0", "v0.2.0"));
  CHECK(rel_newer("v0.3.0", "v0.2.0-57-g53a4a5a-dirty"));   // a build after v0.2.0
  CHECK(!rel_newer("v0.2.0", "v0.2.0-57-g53a4a5a-dirty"));  // the same release: that build is newer or equal
  CHECK(!rel_newer("v0.2.0", "v0.2.0"));
  CHECK(!rel_newer("v0.1.9", "v0.2.0"));
  CHECK(rel_newer("v0.10.0", "v0.9.9"));                    // numbers, not text
  CHECK(rel_newer("v1.0.0", "v0.99.99"));
  CHECK(rel_newer("v0.2.1", "v0.2.0"));
  CHECK(!rel_newer("v0.3.0", ""));                          // running version not known: never offered
  CHECK(!rel_newer("v0.3.0", "53a4a5a"));
  CHECK(!rel_newer("v0.3.0-rc1", "v0.2.0"));                // only plain releases
  CHECK(!rel_newer(NULL, "v0.2.0"));
  return report("update_test");
}
