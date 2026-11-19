// Shared by the host tests: CHECK counts failures without stopping, slurp reads a whole fixture file (exits with a
// message when it is missing), report prints the result and gives main's exit code.
#pragma once
#include <stdio.h>
#include <stdlib.h>

static int fails = 0;
#define CHECK(c) do { if (!(c)) { printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #c); fails++; } } while (0)

// the file's bytes plus a 0 terminator; never freed (a test runs once)
static inline char *slurp(const char *path, size_t *len = NULL) {
  FILE *f = fopen(path, "rb");
  if (!f) { perror(path); exit(1); }
  fseek(f, 0, SEEK_END);
  const long size = ftell(f);
  fseek(f, 0, SEEK_SET);
  char *buf = (char *)malloc((size_t)size + 1);
  const size_t n = buf ? fread(buf, 1, (size_t)size, f) : 0;
  fclose(f);
  if (!buf || n != (size_t)size) { fprintf(stderr, "%s: could not read\n", path); exit(1); }
  buf[n] = 0;
  if (len) *len = n;
  return buf;
}

static inline int report(const char *name) {
  printf(fails ? "%s: %d FAILED\n" : "%s: all passed\n", name, fails);
  return fails != 0;
}
