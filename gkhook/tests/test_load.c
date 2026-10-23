// dlopen the host build; the constructor must log its init line and,
// since this process is not chitu, stay inactive.
#include <dlfcn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define LOG "/tmp/gkhook-test.log"

static int log_has(const char* needle) {
  char buf[4096];
  FILE* f = fopen(LOG, "r");
  if (!f) return 0;
  size_t n = fread(buf, 1, sizeof buf - 1, f);
  fclose(f);
  buf[n] = 0;
  return strstr(buf, needle) != NULL;
}

int main(int argc, char** argv) {
  const char* lib = argc > 1 ? argv[1] : "build/libgkhook_host.so";
  unlink(LOG);
  setenv("GKHOOK_LOG", LOG, 1);
  void* h = dlopen(lib, RTLD_NOW);
  if (!h) { fprintf(stderr, "FAIL dlopen: %s\n", dlerror()); return 1; }
  if (!log_has("gkhook: init")) { fprintf(stderr, "FAIL: no init line in " LOG "\n"); return 1; }
  if (!log_has("inactive: not chitu")) { fprintf(stderr, "FAIL: no inactive line\n"); return 1; }
  dlclose(h);
  puts("test_load OK");
  return 0;
}
