// libgkhook: LD_PRELOAD control hook for chitu V1.3.3.
// Task 1: loads harmlessly. Constructor logs and, if not chitu, returns.
#include <stdarg.h>
#include <stdio.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include "chitu_abi.h"
#include "fingerprint.h"
#include "log.h"
#include "server.h"
#include "tick.h"
#include "validate.h"
#include "zjog.h"

#define CHITU_PATH "/customer/resources/chitu"

__attribute__((visibility("hidden"))) int gk_abi_enabled = 0;

void gk_log(const char* fmt, ...) {
  const char* path = getenv("GKHOOK_LOG");
  FILE* f = fopen(path && *path ? path : "/tmp/gkhook.log", "a");
  if (!f) return;
  fprintf(f, "%ld ", (long)time(NULL));
  va_list ap;
  va_start(ap, fmt);
  vfprintf(f, fmt, ap);
  va_end(ap);
  fputc('\n', f);
  fclose(f);
}

static int is_chitu(void) {
  struct stat a, b;
  if (stat("/proc/self/exe", &a) != 0 || stat(CHITU_PATH, &b) != 0) return 0;
  return a.st_dev == b.st_dev && a.st_ino == b.st_ino;
}

// Reads live memory via /proc/self/mem; a bad address fails instead of faulting.
static int live_peek(uintptr_t addr, unsigned char* out, size_t n) {
  int fd = open("/proc/self/mem", O_RDONLY);
  if (fd < 0) return -1;
  ssize_t r = pread(fd, out, n, (off_t)addr);
  close(fd);
  return r == (ssize_t)n ? 0 : -1;
}

// Waits for app_init to register the stock update callback, then swaps in gk_tick with a
// single atomic store (compare-and-swap). Anything unexpected: log and leave chitu alone.
static void* installer(void* arg) {
  (void)arg;
  volatile uint32_t* slot = (volatile uint32_t*)GK_UI_UPDATE_CB;
  int warned = 0;
  for (int i = 0;; i++) {  // 50 ms for the first ~60 s, then 500 ms for good
    uint32_t cur = *slot;
    if (cur == GK_APP_UPDATE) {
      if (__sync_bool_compare_and_swap(slot, (uint32_t)GK_APP_UPDATE, (uint32_t)(uintptr_t)&gk_tick)) {
        gk_log("gkhook: tick installed");
        // board_init has run (the callback is registered): the axis table must point at axis 0
        int ok = gk_rd32(GK_AXIS_TABLE) == GK_AXIS0_OBJ;
        __atomic_store_n(&gk_z_enabled, ok, __ATOMIC_RELEASE);
        gk_log(ok ? "gkhook: z control enabled" : "gkhook: z control disabled: axis table unexpected");
      } else {
        gk_log("gkhook: tick not installed: callback changed under us");
      }
      return NULL;
    }
    if (cur != 0) {  // set, but not to the stock callback
      gk_log("gkhook: tick not installed: ui.update_cb=0x%x", (unsigned)cur);
      return NULL;
    }
    if (i >= 1200 && !warned) {
      warned = 1;
      gk_log("gkhook: ui.update_cb still unset after 60 s; polling every 500 ms");
    }
    usleep(i >= 1200 ? 500000 : 50000);
  }
}

static void start_installer(void) {
  pthread_attr_t a;
  pthread_t t;
  pthread_attr_init(&a);
  pthread_attr_setdetachstate(&a, PTHREAD_CREATE_DETACHED);
  pthread_attr_setstacksize(&a, 64 * 1024);
  if (pthread_create(&t, &a, installer, NULL) != 0) gk_log("gkhook: cannot start installer thread");
  pthread_attr_destroy(&a);
}

__attribute__((constructor)) void gkhook_init(void) {
  const char* sock = getenv("GKHOOK_SOCK");
  const char* dir = getenv("GKHOOK_FILESDIR");
  gk_log("gkhook: init sock=%s filesdir=%s", sock ? sock : "/tmp/gkhook.sock",
       dir ? dir : "/media/emmc");
  if (!is_chitu()) {
    gk_log("gkhook: inactive: not chitu");
    return;
  }
  unsetenv("LD_PRELOAD");  // children (system, mount, sh) must not inherit us
  gk_abi_enabled = verify_fingerprint("/proc/self/exe", live_peek);
  gk_log(gk_abi_enabled ? "gkhook: chitu V1.3.3 fingerprint match"
                      : "gkhook: wrong chitu version; refusing to act");
  if (gk_abi_enabled) {
    gk_start_ops.cheap = gk_validate_cheap;
    gk_start_ops.full = gk_validate_full;
    gk_start_ops.magic = gk_validate_magic;
    start_installer();
  }
  // Serve whether or not the fingerprint matched: a mismatch answers "wrong chitu version".
  if (gk_server_start(sock && *sock ? sock : "/tmp/gkhook.sock") != 0)
    gk_log("gkhook: cannot start server thread");
}
