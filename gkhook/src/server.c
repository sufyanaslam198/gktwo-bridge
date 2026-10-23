// Unix-socket control server. The accept loop runs on its own thread; each connection is
// handled on a short-lived thread of its own. Neither touches chitu: only the queue module.
#include "server.h"

#include <errno.h>
#include <poll.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <time.h>
#include <unistd.h>

#include "chitu_abi.h"
#include "log.h"
#include "proto.h"
#include "queue.h"

#define REQ_MAX 2048
#define READ_MS 1000     // whole-request deadline: a hung client costs at most this
#define WAIT_MS 2000     // pause/resume/stop wait for the tick
#define MAX_CONN 8       // connections served at once; more get "hook busy"
#define THREAD_STACK (256 * 1024)  // every server-side thread, as the checker that runs chitu's file parser

__attribute__((visibility("hidden"))) struct gk_start_ops gk_start_ops;

static char sock_path[sizeof(((struct sockaddr_un*)0)->sun_path)];
static char files_dir[256];
static atomic_int nconn;                     // live connection threads
static pthread_mutex_t start_mu = PTHREAD_MUTEX_INITIALIZER;  // one start request at a time

static int64_t now_ms(void) {
  struct timespec t;
  clock_gettime(CLOCK_MONOTONIC, &t);
  return (int64_t)t.tv_sec * 1000 + t.tv_nsec / 1000000;
}

// Reads up to and including '\n'. Returns length of the line (without \n), or -1.
static int read_line(int fd, char* buf, size_t cap) {
  size_t n = 0;
  int64_t deadline = now_ms() + READ_MS;
  while (n < cap) {
    int64_t left = deadline - now_ms();
    if (left <= 0) return -1;
    struct pollfd p = { fd, POLLIN, 0 };
    int r = poll(&p, 1, (int)left);
    if (r < 0 && errno == EINTR) continue;
    if (r <= 0) return -1;
    ssize_t k = recv(fd, buf + n, cap - n, 0);
    if (k < 0 && (errno == EINTR || errno == EAGAIN)) continue;
    if (k <= 0) return -1;  // closed (or error) before the newline
    char* nl = memchr(buf + n, '\n', (size_t)k);
    n += (size_t)k;
    if (nl) return (int)(nl - buf);
  }
  return -1;  // no newline within the cap
}

static void reply(int fd, const char* b, size_t n) {
  while (n) {
    ssize_t w = send(fd, b, n, MSG_NOSIGNAL);
    if (w < 0 && errno == EINTR) continue;
    if (w <= 0) return;
    b += w;
    n -= (size_t)w;
  }
}

static void reply_err(int fd, const char* msg) {
  char b[PROTO_MAX];
  size_t n = proto_reply_err(b, sizeof b, msg);
  if (n) reply(fd, b, n);
}

// ---- start ----

static int cheap_path_check(const char* path, char* err, size_t errn) {
  size_t dl = strlen(files_dir), pl = strlen(path);
  if (strncmp(path, files_dir, dl) != 0 || path[dl] != '/') { snprintf(err, errn, "file is not in the files folder"); return -1; }
  const char* name = path + dl + 1;
  if (!*name || strchr(name, '/') || !strcmp(name, ".") || !strcmp(name, "..")) { snprintf(err, errn, "bad file path"); return -1; }
  if (pl < 5 || strcasecmp(path + pl - 4, ".ctb") != 0) { snprintf(err, errn, "only .ctb files can be printed"); return -1; }
  struct stat st;
  if (stat(path, &st) != 0 || !S_ISREG(st.st_mode)) { snprintf(err, errn, "file not found"); return -1; }
  return 0;
}

// Detached: runs the full (slow) validation, then hands the start to the tick.
struct chk { char path[1100]; uint32_t gen; };

static void* checker(void* arg) {
  struct chk* k = arg;
  char* path = k->path;
  char err[128] = "file check failed";
  int bad = gk_start_ops.full && gk_start_ops.full(path, err, sizeof err) != 0;
  uint32_t magic = 0;
  if (!bad && gk_start_ops.magic) bad = gk_start_ops.magic(path, &magic, err, sizeof err) != 0;
  if (bad) {
    gkq_end_start_gen(k->gen, err);  // no-op if this start already ended (timeout) or a newer one began
  } else {
    struct gk_cmd c;
    memset(&c, 0, sizeof c);
    c.kind = CMD_START;
    snprintf(c.path, sizeof c.path, "%s", path);
    c.gen = k->gen;
    c.magic = magic;
    if (!gkq_post(&c)) gkq_end_start_gen(k->gen, "printer busy");
    else gkq_checker_done(k->gen);
  }
  free(k);
  return NULL;
}

static int spawn_detached(void* (*fn)(void*), void* arg, size_t stack) {
  pthread_attr_t a;
  pthread_t t;
  pthread_attr_init(&a);
  pthread_attr_setdetachstate(&a, PTHREAD_CREATE_DETACHED);
  pthread_attr_setstacksize(&a, stack);
  int r = pthread_create(&t, &a, fn, arg);
  pthread_attr_destroy(&a);
  return r;
}

static void do_start_locked(int fd, const struct gk_cmd* c) {
  char err[128];
  struct gk_status st;
  if (!gkq_read_status(&st)) { reply_err(fd, "hook starting"); return; }
  if (strcmp(st.state, "idle") && strcmp(st.state, "complete") && strcmp(st.state, "error")) {
    reply_err(fd, "printer busy");
    return;
  }
  if (st.print_window_open) {  // a finished/errored print is still on the printer's screen
    reply_err(fd, strcmp(st.state, "idle") ? "dismiss the finished print on the printer first" : "printer busy");
    return;
  }
  if (st.z_moving) { reply_err(fd, "Z is moving"); return; }
  if (cheap_path_check(c->path, err, sizeof err) != 0 ||
      (gk_start_ops.cheap && gk_start_ops.cheap(c->path, err, sizeof err) != 0)) {
    reply_err(fd, err);
    return;
  }
  uint32_t gen = gkq_begin_start(c->path);
  if (!gen) { reply_err(fd, "printer busy"); return; }
  struct chk* p = malloc(sizeof *p);
  if (p) {
    snprintf(p->path, sizeof p->path, "%s", c->path);
    p->gen = gen;
  }
  if (!p || spawn_detached(checker, p, THREAD_STACK) != 0) {
    free(p);
    gkq_end_start_gen(gen, "cannot start file check");
    reply_err(fd, "cannot start file check");
    return;
  }
  char b[PROTO_MAX];
  size_t n = proto_reply_ok(b, sizeof b);
  if (n) reply(fd, b, n);
}

// Serialized: chitu's file parser runs inside the validators, and two starts must not race
// each other through the checks (gkq_begin_start would refuse the second one anyway).
static void do_start(int fd, const struct gk_cmd* c) {
  pthread_mutex_lock(&start_mu);
  do_start_locked(fd, c);
  pthread_mutex_unlock(&start_mu);
}

static void handle(int fd) {
  char line[REQ_MAX + 1];
  char b[PROTO_MAX];
  struct gk_cmd c;
  int n = read_line(fd, line, REQ_MAX);
  if (n < 0) return;  // dropped: hung, truncated or oversized client
  int pr = proto_parse(line, (size_t)n, &c);
  if (!gk_abi_enabled) { reply_err(fd, "wrong chitu version"); return; }
  if (pr == -2) { reply_err(fd, "unknown command"); return; }
  if (pr != 0) { reply_err(fd, "bad request"); return; }
  size_t len;
  switch (c.kind) {
    case CMD_STATUS: {
      struct gk_status st;
      if (!gkq_read_status(&st)) { reply_err(fd, "hook starting"); return; }
      len = proto_reply_status(b, sizeof b, &st);
      if (len) reply(fd, b, len); else reply_err(fd, "status unavailable");
      return;
    }
    case CMD_START:
      do_start(fd, &c);
      return;
    default: {  // pause/resume/stop and z_*: the tick decides
      char err[64];
      if (gkq_post_wait(&c, WAIT_MS, err, sizeof err)) {
        len = proto_reply_ok(b, sizeof b);
        if (len) reply(fd, b, len);
      } else {
        reply_err(fd, err);
      }
    }
  }
}

static void* conn_main(void* arg) {
  int fd = (int)(intptr_t)arg;
  handle(fd);
  close(fd);
  atomic_fetch_sub(&nconn, 1);
  return NULL;
}

// Accept thread only: never blocks (non-blocking send of a short reply).
static void refuse_busy(int fd) {
  char b[PROTO_MAX];
  size_t n = proto_reply_err(b, sizeof b, "hook busy");
  if (n) send(fd, b, n, MSG_NOSIGNAL | MSG_DONTWAIT);
  close(fd);
}

static void* serve(void* arg) {
  (void)arg;
  struct sockaddr_un sa;
  memset(&sa, 0, sizeof sa);
  sa.sun_family = AF_UNIX;
  memcpy(sa.sun_path, sock_path, strlen(sock_path));
  int ls = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
  if (ls < 0) { gk_log("gkhook: socket: %s", strerror(errno)); return NULL; }
  unlink(sock_path);  // rebind over a stale socket file
  if (bind(ls, (struct sockaddr*)&sa, sizeof sa) != 0 || listen(ls, 16) != 0) {
    gk_log("gkhook: bind/listen %s: %s", sock_path, strerror(errno));
    close(ls);
    return NULL;
  }
  if (chmod(sock_path, 0600) != 0) gk_log("gkhook: chmod %s: %s", sock_path, strerror(errno));
  for (;;) {
    int fd = accept4(ls, NULL, NULL, SOCK_CLOEXEC);
    if (fd < 0) {
      if (errno != EINTR && errno != ECONNABORTED) usleep(100000);  // EMFILE etc: don't spin
      continue;
    }
    if (atomic_fetch_add(&nconn, 1) >= MAX_CONN ||
        spawn_detached(conn_main, (void*)(intptr_t)fd, THREAD_STACK) != 0) {
      atomic_fetch_sub(&nconn, 1);
      refuse_busy(fd);
    }
  }
  return NULL;
}

int gk_server_start(const char* path) {
  const char* d = getenv("GKHOOK_FILESDIR");
  if (!path || strlen(path) >= sizeof sock_path) return -1;
  snprintf(sock_path, sizeof sock_path, "%s", path);
  snprintf(files_dir, sizeof files_dir, "%s", d && *d ? d : "/media/emmc");
  size_t l = strlen(files_dir);
  while (l > 1 && files_dir[l - 1] == '/') files_dir[--l] = 0;
  return spawn_detached(serve, NULL, THREAD_STACK) == 0 ? 0 : -1;
}
