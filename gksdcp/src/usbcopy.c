#include "usbcopy.h"
#include "files.h"
#include "hookw.h"
#include "loop.h"
#include "sdcp.h"
#include "status.h"
#include "upload.h"
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#define STEP_BYTES (256 * 1024)
#define POLL_MS 2000
#define MOUNT_CHECK_MS 250

static struct {
  int active;
  int src, dst;
  char tmp[600], fin[600];
  long long done, total;
  long long next_ms;                  // earliest start of the next step
  void (*cb)(int ok, const char *err);
} C = { .src = -1, .dst = -1 };

static int g_step_ms;                 // pause between steps, tests only (see usbcopy_init)
static int g_last_mounted;
static long long g_mount_check_ms;    // last usb_mounted() check made by a copy step

static long long now_ms(void) {
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return (long long)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

static int check_fd(int fd, long long *size) {
  struct stat sb;
  unsigned char m[4];
  if (fstat(fd, &sb) < 0 || !S_ISREG(sb.st_mode)) return -1;
  if (pread(fd, m, 4, 0) != 4) return -2;
  uint32_t v = (uint32_t)m[0] | (uint32_t)m[1] << 8 | (uint32_t)m[2] << 16 | (uint32_t)m[3] << 24;
  if (v != 0x12fd0106u && v != 0x12fd0107u) return -2;
  if (size) *size = sb.st_size;
  return 0;
}

int usbcopy_probe(const char *src_abs, long long *size) {
  int fd = open(src_abs, O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
  if (fd < 0) return -1;
  int rc = check_fd(fd, size);
  close(fd);
  return rc;
}

// NULL when an existing file may be replaced now, else the reason.
static const char *replace_refusal(void) {
  struct hk_status h;
  hookw_status(&h);
  if (!h.hook_ok) return "printer state unknown";
  int ok = !strcmp(h.state, "idle") || !strcmp(h.state, "complete") || !strcmp(h.state, "error");
  return ok ? NULL : "printer busy";
}

// Closes both files; the temp file goes too unless it was renamed.
static void teardown(int remove_tmp) {
  if (C.src >= 0) close(C.src);
  if (C.dst >= 0) close(C.dst);
  C.src = C.dst = -1;
  if (remove_tmp) unlink(C.tmp);
  C.active = 0;
}

static void finish(int ok, const char *err, int remove_tmp) {
  void (*cb)(int, const char *) = C.cb;
  teardown(remove_tmp);
  C.cb = NULL;
  if (cb) cb(ok, err);
}

int usbcopy_begin(const char *src_abs, const char *dst_name, void (*done)(int ok, const char *err),
                  const char **why) {
  if (C.active || upload_busy()) { *why = "printer busy"; return -2; }
  if (!upload_storage_ok()) { *why = "storage not mounted"; return -2; }   // a copy on tmpfs is lost
  if (!name_valid(dst_name)) { *why = "bad file path"; return -1; }
  int src = open(src_abs, O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
  long long size = 0;
  int rc = src < 0 ? -1 : check_fd(src, &size);
  if (rc < 0) {
    if (src >= 0) close(src);
    *why = rc == -2 ? "not a ctb file" : "cannot read file";
    return -1;
  }
  const char *dir = files_root(ST_LOCAL);
  if (!files_space_ok(dir, size)) { close(src); *why = "not enough space on internal storage"; return -1; }
  if (snprintf(C.fin, sizeof C.fin, "%s/%s", dir, dst_name) >= (int)sizeof C.fin ||
      snprintf(C.tmp, sizeof C.tmp, "%s/.%s.usb.part", dir, dst_name) >= (int)sizeof C.tmp) {
    close(src);
    *why = "bad file path";
    return -1;
  }
  struct stat st;
  if (lstat(C.fin, &st) == 0 && (*why = S_ISREG(st.st_mode) ? replace_refusal() : "printer busy") != NULL) {
    close(src);
    return -2;
  }
  int dst = open(C.tmp, O_WRONLY | O_CREAT | O_NOFOLLOW | O_CLOEXEC, 0644);
  if (dst >= 0 && flock(dst, LOCK_EX | LOCK_NB) < 0) { close(dst); dst = -1; }   // someone else's live temp file
  if (dst < 0 || ftruncate(dst, 0) < 0) {
    if (dst >= 0) { close(dst); unlink(C.tmp); }
    close(src);
    *why = "cannot write to internal storage";
    return -1;
  }
  C.src = src;
  C.dst = dst;
  C.done = 0;
  C.total = size;
  C.next_ms = 0;
  g_mount_check_ms = now_ms();
  C.cb = done;
  C.active = 1;
  return 0;
}

void usbcopy_cancel(void) {
  if (C.active) finish(0, "cancelled", 1);
}

int usbcopy_active(long long *done, long long *total) {
  if (done) *done = C.done;
  if (total) *total = C.total;
  return C.active;
}

// One step: up to STEP_BYTES from the drive to the temp file; the last step also publishes the file.
static void step(void) {
  static char buf[STEP_BYTES];
  long long now = now_ms();
  if (now - g_mount_check_ms >= MOUNT_CHECK_MS) {      // a pulled drive also shows as read errors
    g_mount_check_ms = now;
    if (!usb_mounted()) { finish(0, "USB drive removed during copy", 1); return; }
  }
  size_t want = (size_t)(C.total - C.done < STEP_BYTES ? C.total - C.done : STEP_BYTES), got = 0;
  while (got < want) {
    ssize_t r = read(C.src, buf + got, want - got);
    if (r < 0 && errno == EINTR) continue;
    if (r < 0) { finish(0, "cannot read from USB drive", 1); return; }
    if (r == 0) { finish(0, "USB file changed during copy", 1); return; }
    got += (size_t)r;
  }
  for (size_t off = 0; off < got;) {
    ssize_t w = write(C.dst, buf + off, got - off);
    if (w < 0 && errno == EINTR) continue;
    if (w <= 0) { finish(0, "cannot write to internal storage", 1); return; }
    off += (size_t)w;
  }
  C.done += (long long)got;
  if (C.done < C.total) return;

  struct stat st;
  const char *why;
  if (lstat(C.fin, &st) == 0 && (why = replace_refusal()) != NULL) { finish(0, why, 1); return; }
  if (fsync(C.dst) < 0) { finish(0, "cannot write to internal storage", 1); return; }
  if (rename(C.tmp, C.fin) < 0) { finish(0, "cannot write to internal storage", 1); return; }
  finish(1, NULL, 0);
}

static int idle(void) {
  if (!C.active) return -1;
  long long now = now_ms();
  if (now < C.next_ms) return (int)(C.next_ms - now);
  step();
  if (!C.active) return -1;
  C.next_ms = now + g_step_ms;
  return g_step_ms;
}

static void usb_poll(void) {
  int m = usb_mounted();
  if (m != g_last_mounted) {
    g_last_mounted = m;
    sdcp_attributes_dirty();
  }
}

void usbcopy_init(void) {
#ifndef NDEBUG
  const char *e = getenv("GKSDCP_TEST_USB_STEP_MS");     // slows the copy down so tests can interrupt it
  if (e) g_step_ms = atoi(e);
#endif
  g_last_mounted = usb_mounted();
  loop_on_idle(idle);
  loop_every(POLL_MS, usb_poll);
}
