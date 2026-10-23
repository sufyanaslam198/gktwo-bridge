#include "ident.h"
#include <arpa/inet.h>
#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#ifndef FW_VERSION
#define FW_VERSION "V0.0.0"
#endif

static char g_id[17] = "0000000000000001";
static char g_name[GK_NAME_MAX + 1] = GK_NAME_DEFAULT;
static char g_dir[256];
static char g_iface[32];
static char g_sysfs[128] = "/sys/class/net";
static int g_fallback;          // 1 while the MAC could not be read and no override is set
static long long g_next_try_ms;

void ident_set_sysfs_root(const char *dir) { snprintf(g_sysfs, sizeof g_sysfs, "%s", dir); }

// Reads the MAC of g_iface into g_id. 0 ok / -1 unreadable.
static int read_mac(void) {
  char path[200], mac[64], id[17];
  snprintf(path, sizeof path, "%s/%s/address", g_sysfs, g_iface);
  FILE *f = g_iface[0] ? fopen(path, "r") : NULL;
  if (!f) return -1;
  int ok = fgets(mac, sizeof mac, f) && ident_id_from_mac(mac, id) == 0;
  fclose(f);
  if (!ok) return -1;
  memcpy(g_id, id, sizeof id);
  return 0;
}

int ident_id_from_mac(const char *mac, char out[17]) {
  char hex[13];
  int n = 0;
  const char *p = mac;
  for (int i = 0; i < 6; i++) {
    if (i && *p++ != ':') return -1;
    if (!isxdigit((unsigned char)p[0]) || !isxdigit((unsigned char)p[1])) return -1;
    hex[n++] = (char)tolower((unsigned char)p[0]);
    hex[n++] = (char)tolower((unsigned char)p[1]);
    p += 2;
  }
  while (*p == '\n' || *p == '\r' || *p == ' ') p++;
  if (*p) return -1;
  hex[n] = 0;
  snprintf(out, 17, "0000%s", hex);
  return 0;
}

// Strict UTF-8: no overlong forms, surrogates or code points above U+10FFFF; no C0 controls or DEL.
int ident_utf8_ok(const char *s) {
  const unsigned char *p = (const unsigned char *)s;
  while (*p) {
    unsigned c = *p;
    if (c < 0x20 || c == 0x7f) return 0;
    if (c < 0x80) { p++; continue; }
    int len;
    unsigned cp, min;
    if ((c & 0xe0) == 0xc0) { len = 2; cp = c & 0x1f; min = 0x80; }
    else if ((c & 0xf0) == 0xe0) { len = 3; cp = c & 0x0f; min = 0x800; }
    else if ((c & 0xf8) == 0xf0) { len = 4; cp = c & 0x07; min = 0x10000; }
    else return 0;
    for (int i = 1; i < len; i++) {
      if ((p[i] & 0xc0) != 0x80) return 0;
      cp = (cp << 6) | (p[i] & 0x3f);
    }
    if (cp < min || cp > 0x10ffff || (cp >= 0xd800 && cp <= 0xdfff)) return 0;
    p += len;
  }
  return 1;
}

static int name_valid(const char *s) {
  size_t n = strlen(s);
  return n >= 1 && n <= GK_NAME_MAX && ident_utf8_ok(s);
}

static void load_name(void) {
  char path[300], buf[GK_NAME_MAX + 2];
  snprintf(path, sizeof path, "%s/name", g_dir);
  int fd = open(path, O_RDONLY | O_CLOEXEC);
  if (fd < 0) return;
  ssize_t n = read(fd, buf, sizeof buf - 1);
  close(fd);
  if (n <= 0) return;
  buf[n] = 0;
  if (!name_valid(buf)) {
    fprintf(stderr, "gksdcp: ignoring invalid %s\n", path);
    return;
  }
  strcpy(g_name, buf);
}

int ident_init(const char *iface, const char *override_id, const char *var_dir) {
  snprintf(g_dir, sizeof g_dir, "%s", var_dir);
  strcpy(g_name, GK_NAME_DEFAULT);
  if (override_id) {
    if (strlen(override_id) != 16) return -1;
    for (int i = 0; i < 16; i++) if (!isxdigit((unsigned char)override_id[i])) return -1;
    for (int i = 0; i < 16; i++) g_id[i] = (char)tolower((unsigned char)override_id[i]);
    g_id[16] = 0;
    g_fallback = 0;
  } else {
    snprintf(g_iface, sizeof g_iface, "%s", iface ? iface : "");
    g_fallback = read_mac() < 0;
    if (g_fallback) {
      fprintf(stderr, "gksdcp: cannot read MAC of %s, using MainboardID 0000000000000001 until it appears\n",
              g_iface[0] ? g_iface : "(none)");
      strcpy(g_id, "0000000000000001");
    }
    g_next_try_ms = 0;
  }
  load_name();
  return 0;
}

const char *ident_mainboard_id(void) {
  if (g_fallback) {     // the interface may register after start-up: retry at most once a second
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    long long now = ts.tv_sec * 1000LL + ts.tv_nsec / 1000000;
    if (now >= g_next_try_ms) {
      g_next_try_ms = now + 1000;
      if (read_mac() == 0) {
        g_fallback = 0;
        fprintf(stderr, "gksdcp: MAC of %s now readable, MainboardID is %s\n", g_iface, g_id);
      }
    }
  }
  return g_id;
}
const char *ident_name(void) { return g_name; }

int ident_mkdir_p(const char *dir) {
  char tmp[256];
  snprintf(tmp, sizeof tmp, "%s", dir);
  for (char *p = tmp + 1; *p; p++) {
    if (*p != '/') continue;
    *p = 0;
    if (mkdir(tmp, 0755) < 0 && errno != EEXIST) return -1;
    *p = '/';
  }
  if (mkdir(tmp, 0755) < 0 && errno != EEXIST) return -1;
  return 0;
}

int ident_set_name(const char *name) {
  if (!name_valid(name)) return -1;
  if (ident_mkdir_p(g_dir) < 0) return -1;
  char path[300], tmp[310];
  snprintf(path, sizeof path, "%s/name", g_dir);
  snprintf(tmp, sizeof tmp, "%s.tmp", path);
  int fd = open(tmp, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
  if (fd < 0) return -1;
  size_t n = strlen(name);
  int ok = write(fd, name, n) == (ssize_t)n && fsync(fd) == 0;
  if (close(fd) < 0) ok = 0;
  if (!ok || rename(tmp, path) < 0) { unlink(tmp); return -1; }
  strcpy(g_name, name);
  return 0;
}

const char *ident_fw_version(void) { return FW_VERSION "-bridge-" BRIDGE_VERSION; }

int ident_ip_for(const struct in_addr *local, char out[16]) {
  return inet_ntop(AF_INET, local, out, 16) ? 0 : -1;
}
