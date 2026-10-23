// .ctb pre-validation. An invalid file makes chitu's printer_create return NULL and then
// crash (findings 2, "latent crash"), so everything the stock info dialog checks is
// replicated here before a print is ever requested.
#include "validate.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <unistd.h>

#include "chitu_abi.h"

#define CTB_MAGIC_A 0x12fd0106u  // findings 2
#define CTB_MAGIC_B 0x12fd0107u

// print_file field offsets (findings 2): X res, Y res (u32 each), format/version (u32).
#define F_XRES 0x1004
#define F_YRES 0x1008
#define F_VER  0x1e10

static void msg(char* err, size_t n, const char* m) { snprintf(err, n, "%s", m); }

int gk_file_magic(const char* path, uint32_t* out) {
  unsigned char b[4];
  int fd = open(path, O_RDONLY | O_CLOEXEC);
  if (fd < 0) return -1;
  ssize_t r;
  do r = read(fd, b, sizeof b); while (r < 0 && errno == EINTR);
  close(fd);
  if (r != (ssize_t)sizeof b) return -1;
  *out = (uint32_t)b[0] | (uint32_t)b[1] << 8 | (uint32_t)b[2] << 16 | (uint32_t)b[3] << 24;
  return 0;
}

int gk_validate_magic(const char* path, uint32_t* magic, char* err, size_t errn) {
  uint32_t m;
  if (gk_file_magic(path, &m) != 0) { msg(err, errn, "cannot read file"); return -1; }
  if (m != CTB_MAGIC_A && m != CTB_MAGIC_B) { msg(err, errn, "not a ctb file"); return -1; }
  *magic = m;
  return 0;
}

int gk_validate_cheap(const char* path, char* err, size_t errn) {
  uint32_t magic;
  if (gk_file_magic(path, &magic) != 0) { msg(err, errn, "cannot read file"); return -1; }
  if (magic != CTB_MAGIC_A && magic != CTB_MAGIC_B) { msg(err, errn, "not a ctb file"); return -1; }
  // Only now hand the file to chitu's parser.
  if (gk_print_file_get_format(path) == 0) { msg(err, errn, "unsupported file format"); return -1; }
  void* f = gk_print_file_open(path);
  if (!f) { msg(err, errn, "cannot open file"); return -1; }
  const unsigned char* b = f;
  uint32_t x = *(const uint32_t*)(b + F_XRES), y = *(const uint32_t*)(b + F_YRES);
  uint32_t ver = *(const uint32_t*)(b + F_VER);
  int rc = 0;
  if (x != gk_rd16(GK_RES_X) || y != gk_rd16(GK_RES_Y)) { msg(err, errn, "file resolution does not match printer"); rc = -1; }
  else if (ver != 1 && ver != 6) { msg(err, errn, "unsupported ctb version"); rc = -1; }
  gk_print_file_close(f);
  return rc;
}

int gk_validate_full(const char* path, char* err, size_t errn) {
  void* f = gk_print_file_open(path);
  if (!f) { msg(err, errn, "cannot open file"); return -1; }
  // print_file_check returns 0 on success: app_explorer_updata 0x14710-0x14728 loads
  // check_file_normal_result (0xff320), `cmp r2,#0 ; beq 0x1499c` (success path ->
  // print_file_tip dialog); -1 (open failed, 0x14338) -> error dialog 0x1496c; -2 = pending.
  int rc = gk_print_file_check(f);
  gk_print_file_close(f);
  if (rc != 0) { msg(err, errn, "file check failed"); return -1; }
  return 0;
}
