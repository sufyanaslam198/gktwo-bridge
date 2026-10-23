#include "files.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define CHECK(c) do { if (!(c)) { fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #c); exit(1); } } while (0)

static void test_name_valid(void) {
  CHECK(name_valid("a.ctb"));
  CHECK(name_valid("My part (2).ctb"));
  CHECK(name_valid("#1 v2+fix.ctb"));
  CHECK(name_valid("Drache_\xc3\x9c (2).ctb"));
  CHECK(!name_valid(""));
  CHECK(!name_valid("."));
  CHECK(!name_valid(".."));
  CHECK(!name_valid(".x.ctb"));
  CHECK(!name_valid("a/b"));
  CHECK(!name_valid("a\\b"));
  CHECK(!name_valid("a:b"));
  CHECK(!name_valid("a*b"));
  CHECK(!name_valid("a?b"));
  CHECK(!name_valid("a\"b"));
  CHECK(!name_valid("a<b"));
  CHECK(!name_valid("a>b"));
  CHECK(!name_valid("a|b"));
  CHECK(!name_valid("a\x01"));
  CHECK(!name_valid("a\x7f"));
  CHECK(!name_valid("\xc3\x28"));          // invalid UTF-8
  CHECK(!name_valid("\xc0\x80"));          // overlong
  char big[210];
  memset(big, 'x', 201); big[201] = 0;
  CHECK(!name_valid(big));
  big[200] = 0;
  CHECK(name_valid(big));
}

static void test_resolve(const char *root, const char *usb) {
  char abs[512], want[512];
  int st = -1;
  snprintf(want, sizeof want, "%s/a.ctb", root);
  CHECK(files_resolve("/local/a.ctb", abs, sizeof abs, &st) == 0 && strcmp(abs, want) == 0 && st == ST_LOCAL);
  CHECK(files_resolve("a.ctb", abs, sizeof abs, &st) == 0 && strcmp(abs, want) == 0 && st == ST_LOCAL);
  CHECK(files_resolve("/local/", abs, sizeof abs, &st) == 0 && strcmp(abs, root) == 0);
  CHECK(files_resolve("/local/dir/", abs, sizeof abs, &st) == 0);       // existing directory: trailing slash ok
  CHECK(files_resolve("/local/dir//", abs, sizeof abs, &st) == 0);
  CHECK(files_resolve("/local/nodir/", abs, sizeof abs, &st) == -1);    // not a directory
  CHECK(files_resolve("/local/f.ctb/", abs, sizeof abs, &st) == -1);    // a file with a trailing slash
  snprintf(want, sizeof want, "%s/d/a.ctb", usb);
  CHECK(files_resolve("/usb/d/a.ctb", abs, sizeof abs, &st) == 0 && strcmp(abs, want) == 0 && st == ST_USB);
  CHECK(files_resolve("/local/../x", abs, sizeof abs, &st) == -1);
  CHECK(files_resolve("/other/x", abs, sizeof abs, &st) == -1);
  CHECK(files_resolve("/local/./x", abs, sizeof abs, &st) == -1);
  CHECK(files_resolve("", abs, sizeof abs, &st) == -1);
  CHECK(files_resolve("/localx/a.ctb", abs, sizeof abs, &st) == -1);
  CHECK(files_resolve("/local/a\\b", abs, sizeof abs, &st) == -1);
  CHECK(files_resolve("/local/a.ctb", abs, 8, &st) == -1);              // does not fit
}

static void test_client_path(const char *root, const char *usb) {
  char abs[512], out[512], in[600];
  CHECK(files_client_path(root, out, sizeof out) == 0 && strcmp(out, "/local/") == 0);
  snprintf(in, sizeof in, "%s/d/a.ctb", root);
  CHECK(files_client_path(in, out, sizeof out) == 0 && strcmp(out, "/local/d/a.ctb") == 0);
  snprintf(in, sizeof in, "%s/a.ctb", usb);
  CHECK(files_client_path(in, out, sizeof out) == 0 && strcmp(out, "/usb/a.ctb") == 0);
  CHECK(files_client_path("/elsewhere/a.ctb", out, sizeof out) == -1);
  snprintf(in, sizeof in, "%sx/a.ctb", root);                           // sibling with the same prefix
  CHECK(files_client_path(in, out, sizeof out) == -1);
  int st;
  CHECK(files_resolve("/local/d/a.ctb", abs, sizeof abs, &st) == 0);
  CHECK(files_client_path(abs, out, sizeof out) == 0 && strcmp(out, "/local/d/a.ctb") == 0);
}

int main(void) {
  char tmpl[] = "/tmp/test_filesXXXXXX";
  CHECK(mkdtemp(tmpl));
  char root[200], usb[200], mounts[200], p[260];
  snprintf(root, sizeof root, "%s/files", tmpl);
  snprintf(usb, sizeof usb, "%s/usb", tmpl);
  snprintf(mounts, sizeof mounts, "%s/mounts", tmpl);
  CHECK(mkdir(root, 0755) == 0 && mkdir(usb, 0755) == 0);
  snprintf(p, sizeof p, "%s/dir", root); CHECK(mkdir(p, 0755) == 0);
  snprintf(p, sizeof p, "%s/f.ctb", root);
  FILE *f = fopen(p, "w"); CHECK(f); fclose(f);

  test_name_valid();

  char rootslash[210];
  snprintf(rootslash, sizeof rootslash, "%s/", root);                   // trailing slash is normalised
  CHECK(files_init(rootslash, usb, mounts) == 0);
  f = fopen(mounts, "w"); CHECK(f);
  fprintf(f, "/dev/sda1 %s vfat rw 0 0\n", usb);
  fclose(f);
  test_resolve(root, usb);
  test_client_path(root, usb);

  // usb_mounted: second field of a mounts line equals the usb dir
  CHECK(unlink(mounts) == 0);
  CHECK(usb_mounted() == 0);                                            // no mounts file
  f = fopen(mounts, "w"); CHECK(f);
  fprintf(f, "proc /proc proc rw 0 0\n/dev/sda1 %sx vfat rw 0 0\n", usb);
  fclose(f);
  CHECK(usb_mounted() == 0);                                            // prefix only
  char sub[300];
  CHECK(files_resolve("/usb/a.ctb", sub, sizeof sub, &(int){0}) == -2);
  f = fopen(mounts, "w"); CHECK(f);
  fprintf(f, "proc /proc proc rw 0 0\n/dev/sda1 %s vfat rw 0 0\n", usb);
  fclose(f);
  CHECK(usb_mounted() == 1);
  CHECK(files_resolve("/usb/a.ctb", sub, sizeof sub, &(int){0}) == 0);
  CHECK(files_resolve("/usb/../x", sub, sizeof sub, &(int){0}) == -1);  // invalid beats not mounted

  // storage checks
  CHECK(files_storage_ok(root, 1) == 1);
  CHECK(files_storage_ok("/nonexistent-dir-xyz", 0) == 0);   // not mounted yet: refused
  CHECK(files_storage_ok("/nonexistent-dir-xyz", 1) == 1);
  CHECK(files_free_bytes(root) > 0);
  CHECK(files_free_bytes("/nonexistent-dir-xyz") == -1);
  CHECK(FILES_RESERVE_BYTES == 16LL * 1024 * 1024);

  char cmd[300];
  snprintf(cmd, sizeof cmd, "rm -rf %s", tmpl);
  if (system(cmd) != 0) return 1;
  return 0;
}
