#include "files.h"
#include "ident.h"
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/statfs.h>
#include <sys/statvfs.h>
#include <sys/vfs.h>

#ifndef TMPFS_MAGIC
#define TMPFS_MAGIC 0x01021994
#endif
#ifndef RAMFS_MAGIC
#define RAMFS_MAGIC 0x858458f6
#endif

#define NAME_MAX_BYTES 200
#define DIR_CAP 256

static char g_files[DIR_CAP], g_usb[DIR_CAP], g_mounts[DIR_CAP];

static int set_dir(char *dst, const char *src) {
  size_t n = strlen(src);
  while (n > 1 && src[n - 1] == '/') n--;
  if (n >= DIR_CAP) return -1;
  memcpy(dst, src, n);
  dst[n] = 0;
  return 0;
}

int files_init(const char *files_dir, const char *usb_dir, const char *mounts_file) {
  if (set_dir(g_files, files_dir) < 0 || set_dir(g_usb, usb_dir) < 0) return -1;
  if (strlen(mounts_file) >= DIR_CAP) return -1;
  strcpy(g_mounts, mounts_file);
  return 0;
}

const char *files_root(int storage) { return storage == ST_USB ? g_usb : g_files; }

int name_valid(const char *name) {
  size_t n = strlen(name);
  if (n < 1 || n > NAME_MAX_BYTES || name[0] == '.') return 0;
  if (strpbrk(name, "/\\:*?\"<>|")) return 0;
  return ident_utf8_ok(name);
}

int usb_mounted(void) {
  FILE *f = fopen(g_mounts, "r");
  if (!f) return 0;
  char line[1024];
  int found = 0;
  while (!found && fgets(line, sizeof line, f)) {
    char *dev = strtok(line, " \t");
    char *mp = dev ? strtok(NULL, " \t") : NULL;
    if (mp && strcmp(mp, g_usb) == 0) found = 1;
  }
  fclose(f);
  return found;
}

int files_resolve(const char *client, char *abs, size_t cap, int *storage) {
  int st = ST_LOCAL;
  const char *rest = client;
  if (client[0] == '/') {
    if (strncmp(client, "/local", 6) == 0 && (client[6] == '/' || !client[6])) rest = client + 6;
    else if (strncmp(client, "/usb", 4) == 0 && (client[4] == '/' || !client[4])) { st = ST_USB; rest = client + 4; }
    else return -1;
    if (*rest == '/') rest++;                  // the separator after the storage name
  } else if (!client[0]) {
    return -1;
  }

  const char *root = files_root(st);
  size_t len = strlen(root);
  if (len >= cap) return -1;
  memcpy(abs, root, len);
  abs[len] = 0;
  int empty_component = 0;
  for (const char *p = rest; *p;) {
    const char *e = strchr(p, '/');
    size_t n = e ? (size_t)(e - p) : strlen(p);
    if (n == 0) {
      empty_component = 1;
    } else {
      char comp[NAME_MAX_BYTES + 2];
      if (n > NAME_MAX_BYTES) return -1;
      memcpy(comp, p, n);
      comp[n] = 0;
      if (!name_valid(comp) || len + 1 + n >= cap) return -1;
      abs[len++] = '/';
      memcpy(abs + len, comp, n);
      len += n;
      abs[len] = 0;
    }
    if (!e) break;
    if (e[1] == 0) empty_component = 1;        // trailing slash
    p = e + 1;
  }
  if (st == ST_USB && !usb_mounted()) return -2;
  if (empty_component) {
    struct stat sb;
    if (stat(abs, &sb) < 0 || !S_ISDIR(sb.st_mode)) return -1;
  }
  if (storage) *storage = st;
  return 0;
}

// abs is root or root/<rest>
static const char *under(const char *abs, const char *root) {
  size_t n = strlen(root);
  if (strncmp(abs, root, n) != 0) return NULL;
  if (abs[n] == 0) return abs + n;
  return abs[n] == '/' ? abs + n + 1 : NULL;
}

int files_client_path(const char *abs, char *out, size_t cap) {
  const char *rest = under(abs, g_files);
  const char *prefix = "/local/";
  if (!rest) { rest = under(abs, g_usb); prefix = "/usb/"; }
  if (!rest) return -1;
  int n = snprintf(out, cap, "%s%s", prefix, rest);
  return n < 0 || (size_t)n >= cap ? -1 : 0;
}

int files_storage_ok(const char *dir, int allow_tmpfs) {
  struct statfs sf;
  if (allow_tmpfs) return 1;
  if (statfs(dir, &sf) != 0) return 0;         // missing dir: the card is not mounted yet (/media/emmc appears later)
  return !((unsigned long)sf.f_type == (unsigned long)TMPFS_MAGIC ||
           (unsigned long)sf.f_type == (unsigned long)RAMFS_MAGIC);
}

long long files_free_bytes(const char *dir) {
  struct statvfs sv;
  if (statvfs(dir, &sv) != 0) return -1;
  return (long long)sv.f_bavail * (long long)sv.f_frsize;
}

static long long g_reserve = FILES_RESERVE_BYTES;

void files_set_reserve(long long bytes) { g_reserve = bytes < 0 ? 0 : bytes; }
long long files_reserve(void) { return g_reserve; }

int files_space_ok(const char *dir, long long bytes) {
  long long free_b = files_free_bytes(dir);
  return free_b >= 0 && free_b >= g_reserve && bytes <= free_b - g_reserve;
}
