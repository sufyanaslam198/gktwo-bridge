#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include "mock_chitu.h"
#include "../src/validate.h"

static int fails;
#define CHECK(c) do { if (!(c)) { fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); fails++; } } while (0)

static char path[64];
static void mkfile(const unsigned char* head, size_t n) {
  FILE* f = fopen(path, "wb");
  fwrite(head, 1, n, f);
  fclose(f);
}

int main(void) {
  char err[128];
  snprintf(path, sizeof path, "/tmp/gkt_val_%d.ctb", (int)getpid());
  const unsigned char ctb6[] = {0x06, 0x01, 0xfd, 0x12, 0, 0, 0, 0};
  const unsigned char ctb7[] = {0x07, 0x01, 0xfd, 0x12, 0, 0, 0, 0};
  const unsigned char junk[] = {'N', 'O', 'P', 'E', 0, 0, 0, 0};

  mock_reset();
  mkfile(ctb6, sizeof ctb6);
  CHECK(gk_validate_cheap(path, err, sizeof err) == 0);
  CHECK(M.n_open == 1 && M.n_close == 1);
  mkfile(ctb7, sizeof ctb7);
  CHECK(gk_validate_cheap(path, err, sizeof err) == 0);

  // not a ctb: chitu's parser is never called
  mock_reset();
  mkfile(junk, sizeof junk);
  CHECK(gk_validate_cheap(path, err, sizeof err) != 0 && strcmp(err, "not a ctb file") == 0);
  CHECK(M.n_getfmt == 0 && M.n_open == 0);

  // unreadable / too short
  mock_reset();
  CHECK(gk_validate_cheap("/tmp/gkt_nonexistent.ctb", err, sizeof err) != 0 && strcmp(err, "cannot read file") == 0);
  mkfile(ctb6, 2);
  CHECK(gk_validate_cheap(path, err, sizeof err) != 0 && strcmp(err, "cannot read file") == 0);

  // format 0, open NULL
  mock_reset(); mkfile(ctb6, sizeof ctb6);
  M.fmt = 0;
  CHECK(gk_validate_cheap(path, err, sizeof err) != 0 && strcmp(err, "unsupported file format") == 0 && M.n_open == 0);
  mock_reset(); mkfile(ctb6, sizeof ctb6);
  M.open_ok = 0;
  CHECK(gk_validate_cheap(path, err, sizeof err) != 0 && strcmp(err, "cannot open file") == 0);

  // resolution X, Y; version; each closes the handle it opened
  mock_reset(); mkfile(ctb6, sizeof ctb6);
  M.fx = 1920;
  CHECK(gk_validate_cheap(path, err, sizeof err) != 0 && strstr(err, "resolution") && M.n_close == 1);
  mock_reset(); mkfile(ctb6, sizeof ctb6);
  M.fy = 1;
  CHECK(gk_validate_cheap(path, err, sizeof err) != 0 && strstr(err, "resolution") && M.n_close == 1);
  mock_reset(); mkfile(ctb6, sizeof ctb6);
  M.fver = 5;
  CHECK(gk_validate_cheap(path, err, sizeof err) != 0 && strcmp(err, "unsupported ctb version") == 0 && M.n_close == 1);
  mock_reset(); mkfile(ctb6, sizeof ctb6);
  M.fver = 1;
  CHECK(gk_validate_cheap(path, err, sizeof err) == 0);

  // magic carried to the tick
  mock_reset(); mkfile(ctb7, sizeof ctb7);
  { uint32_t m = 0; CHECK(gk_validate_magic(path, &m, err, sizeof err) == 0 && m == 0x12fd0107u); }
  mkfile(junk, sizeof junk);
  { uint32_t m = 0; CHECK(gk_validate_magic(path, &m, err, sizeof err) != 0 && !strcmp(err, "not a ctb file") && m == 0); }
  CHECK(gk_validate_magic("/tmp/gkt_nonexistent.ctb", &(uint32_t){0}, err, sizeof err) != 0 && !strcmp(err, "cannot read file"));

  // full check: 0 = success
  mock_reset();
  CHECK(gk_validate_full(path, err, sizeof err) == 0 && M.n_check == 1 && M.n_close == 1);
  M.check_rc = -1;
  CHECK(gk_validate_full(path, err, sizeof err) != 0 && strcmp(err, "file check failed") == 0 && M.n_close == 2);
  M.check_rc = 3;
  CHECK(gk_validate_full(path, err, sizeof err) != 0);
  M.open_ok = 0;
  CHECK(gk_validate_full(path, err, sizeof err) != 0 && strcmp(err, "cannot open file") == 0);

  unlink(path);
  if (fails) { fprintf(stderr, "%d failure(s)\n", fails); return 1; }
  puts("test_validate OK");
  return 0;
}
