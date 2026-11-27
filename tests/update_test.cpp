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
