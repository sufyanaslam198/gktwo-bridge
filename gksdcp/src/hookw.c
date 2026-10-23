#include "hookw.h"
#include "hook_io.h"
#include "json.h"
#include "loop.h"
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#define POLL_MS       250
#define POLL_TIMEOUT  500            // a status poll never holds a queued command for long
#define CALL_TIMEOUT  2300
#define CALL_MAX      16
#define LINE_MAX_LEN  1280
#define REPLY_MAX     2304

struct done {
  struct done *next;
  hookw_done cb;
  void *ctx;
  int ok;
  char err[128];
  unsigned long seq;       // polls completed when the reply came
};

struct call {
  char line[LINE_MAX_LEN];
  struct done *dn;         // completion node, allocated by hookw_call so the worker cannot fail
  long long t0;
};


static pthread_mutex_t mu = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t cv;
static struct hk_status g_snap;
static unsigned long g_seq;                    // completed status polls; g_snap is from the last one
static int g_changed;                          // snapshot changed since the loop last looked
static int g_after_call;                       // worker only: the next poll follows a command
static unsigned long g_cb_seq;                 // loop only: seq of the completion being delivered
static struct call q[CALL_MAX];
static int q_head, q_n;
static struct done *done_head, *done_tail;
static char g_sock[256];
static int g_pipe[2] = {-1, -1};
#define MAX_LISTENERS 8
static void (*g_listeners[MAX_LISTENERS])(void);
static int g_n_listeners;

static long long now_ms(void) {
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return (long long)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

// ---- reply parsing ----

static void unreachable(struct hk_status *s, const char *why) {
  memset(s, 0, sizeof *s);
  s->layer = s->total_layers = s->elapsed_s = s->remaining_s = -1;
  s->screen_x = s->screen_y = -1;
  snprintf(s->state, sizeof s->state, "idle");
  snprintf(s->hook_error, sizeof s->hook_error, "%s", why);
}

static int is_null_tok(const struct jdoc *d, int t) {
  return t >= 0 && d->t[t].type == JSMN_PRIMITIVE && d->js[d->t[t].start] == 'n';
}

static int is_true_tok(const struct jdoc *d, int t) {
  return t >= 0 && d->t[t].type == JSMN_PRIMITIVE && d->js[d->t[t].start] == 't';
}

static long get_long(const struct jdoc *d, int obj, const char *key) {
  long long v;
  int t = jget(d, obj, key);
  return (t >= 0 && jint(d, t, &v) == 0) ? (long)v : -1;
}

static void get_str(const struct jdoc *d, int obj, const char *key, char *out, size_t cap) {
  int t = jget(d, obj, key);
  if (t < 0 || jstr(d, t, out, cap) < 0) out[0] = 0;
}

void hookw_parse_reply(const char *reply, struct hk_status *s) {
  jsmntok_t toks[96];
  struct jdoc d;
  if (jdoc_parse(&d, reply, strlen(reply), toks, 96) < 0 || d.n < 1 || d.t[0].type != JSMN_OBJECT) {
    unreachable(s, "bad status reply");
    return;
  }
  if (!is_true_tok(&d, jget(&d, 0, "ok"))) {
    char err[96] = "";
    get_str(&d, 0, "error", err, sizeof err);
    unreachable(s, err[0] ? err : "hook error");
    return;
  }
  unreachable(s, "");
  s->state[0] = 0;
  get_str(&d, 0, "state", s->state, sizeof s->state);
  if (!s->state[0]) {
    unreachable(s, "bad status reply");
    return;
  }
  s->hook_ok = 1;
  get_str(&d, 0, "file", s->file, sizeof s->file);
  s->layer = get_long(&d, 0, "layer");
  s->total_layers = get_long(&d, 0, "total_layers");
  s->elapsed_s = get_long(&d, 0, "elapsed_s");
  s->remaining_s = get_long(&d, 0, "remaining_s");
  get_str(&d, 0, "phase", s->phase, sizeof s->phase);
  get_str(&d, 0, "message", s->message, sizeof s->message);
  int z = jget(&d, 0, "z");
  if (z >= 0 && d.t[z].type == JSMN_OBJECT) {
    s->z_avail = 1;
    s->z_homed = is_true_tok(&d, jget(&d, z, "homed"));
    s->z_moving = is_true_tok(&d, jget(&d, z, "moving"));
    int p = jget(&d, z, "pos");
    if (p >= 0 && !is_null_tok(&d, p) && d.t[p].type == JSMN_PRIMITIVE) {
      char num[40];
      int n = d.t[p].end - d.t[p].start;
      if (n > 0 && n < (int)sizeof num) {
        memcpy(num, d.js + d.t[p].start, (size_t)n);
        num[n] = 0;
        s->z_pos = strtod(num, NULL);
      }
    }
  }
  int sc = jget(&d, 0, "screen");
  if (jarr_n(&d, sc) == 2) {
    long long x, y;
    if (jint(&d, jarr_at(&d, sc, 0), &x) == 0 && jint(&d, jarr_at(&d, sc, 1), &y) == 0 &&
        x > 0 && y > 0 && x < 100000 && y < 100000) {
      s->screen_x = (int)x;
      s->screen_y = (int)y;
    }
  }
}

// ---- worker ----

static void wake_loop(void) {
  char c = 1;
  ssize_t r = write(g_pipe[1], &c, 1);      // a full pipe already means "wake up"
  (void)r;
}

static void poll_status(void) {
  char reply[REPLY_MAX];
  struct hk_status s;
  if (hook_request(g_sock, "{\"cmd\":\"status\"}", reply, sizeof reply, POLL_TIMEOUT) < 0)
    unreachable(&s, "hook not running");
  else
    hookw_parse_reply(reply, &s);
  // The poll after a command always reaches the listeners: a start can fail before any poll sees it, and
  // the snapshot is then the same as before the start (same message).
  pthread_mutex_lock(&mu);
  int changed = memcmp(&s, &g_snap, sizeof s) != 0 || g_after_call;
  g_seq++;
  if (changed) { g_snap = s; g_changed = 1; }
  pthread_mutex_unlock(&mu);
  g_after_call = 0;
  if (changed) wake_loop();
}

static void run_call(const struct call *c) {
  struct done *dn = c->dn;
  long long left = CALL_TIMEOUT - (now_ms() - c->t0);
  char reply[REPLY_MAX];
  if (left <= 0 || hook_request(g_sock, c->line, reply, sizeof reply, (int)left) < 0) {
    snprintf(dn->err, sizeof dn->err, "printer not responding");
  } else {
    jsmntok_t toks[32];
    struct jdoc d;
    if (jdoc_parse(&d, reply, strlen(reply), toks, 32) < 0 || d.n < 1 || d.t[0].type != JSMN_OBJECT) {
      snprintf(dn->err, sizeof dn->err, "bad reply from printer");
    } else if (is_true_tok(&d, jget(&d, 0, "ok"))) {
      dn->ok = 1;
    } else {
      get_str(&d, 0, "error", dn->err, sizeof dn->err);
      if (!dn->err[0]) snprintf(dn->err, sizeof dn->err, "printer refused");
    }
  }
  g_after_call = 1;
  pthread_mutex_lock(&mu);
  dn->seq = g_seq;
  if (done_tail) done_tail->next = dn; else done_head = dn;
  done_tail = dn;
  pthread_mutex_unlock(&mu);
  wake_loop();
}

static void *worker(void *arg) {
  (void)arg;
  long long next_poll = now_ms() + POLL_MS;
  for (;;) {
    pthread_mutex_lock(&mu);
    struct call c;
    int have = 0;
    for (;;) {
      if (q_n) { c = q[q_head]; q_head = (q_head + 1) % CALL_MAX; q_n--; have = 1; break; }
      long long left = next_poll - now_ms();
      if (left <= 0) break;
      struct timespec ts;
      clock_gettime(CLOCK_MONOTONIC, &ts);
      ts.tv_sec += left / 1000;
      ts.tv_nsec += (left % 1000) * 1000000;
      if (ts.tv_nsec >= 1000000000L) { ts.tv_sec++; ts.tv_nsec -= 1000000000L; }
      pthread_cond_timedwait(&cv, &mu, &ts);
    }
    pthread_mutex_unlock(&mu);
    if (have) {
      run_call(&c);
      next_poll = now_ms();                 // gkhook has a new snapshot at once after a command
    } else {
      poll_status();
      next_poll = now_ms() + POLL_MS;
    }
  }
  return NULL;
}

// ---- loop side ----

static void on_wake(int fd) {
  char buf[64];
  while (read(fd, buf, sizeof buf) > 0) {}
  pthread_mutex_lock(&mu);
  struct done *list = done_head;
  done_head = done_tail = NULL;
  int changed = g_changed;
  g_changed = 0;
  pthread_mutex_unlock(&mu);
  while (list) {
    struct done *n = list->next;
    g_cb_seq = list->seq;
    if (list->cb) list->cb(list->ctx, list->ok, list->err);
    free(list);
    list = n;
  }
  if (changed)
    for (int i = 0; i < g_n_listeners; i++) g_listeners[i]();
}

int hookw_init(const char *sock) {
  if (strlen(sock) >= sizeof g_sock) return -1;
  strcpy(g_sock, sock);
  unreachable(&g_snap, "hook not running");
  pthread_condattr_t ca;
  pthread_condattr_init(&ca);
  pthread_condattr_setclock(&ca, CLOCK_MONOTONIC);
  pthread_cond_init(&cv, &ca);
  if (pipe2(g_pipe, O_NONBLOCK | O_CLOEXEC) < 0) return -1;
  poll_status();                            // a first snapshot before any client can ask
  g_changed = 0;
  loop_add_fd(g_pipe[0], on_wake);
  pthread_t t;
  pthread_attr_t at;
  pthread_attr_init(&at);
  pthread_attr_setdetachstate(&at, PTHREAD_CREATE_DETACHED);
  pthread_attr_setstacksize(&at, 128 * 1024);
  int r = pthread_create(&t, &at, worker, NULL);
  pthread_attr_destroy(&at);
  return r == 0 ? 0 : -1;
}

unsigned long hookw_status(struct hk_status *out) {
  pthread_mutex_lock(&mu);
  *out = g_snap;
  unsigned long seq = g_seq;
  pthread_mutex_unlock(&mu);
  return seq;
}

unsigned long hookw_call_seq(void) { return g_cb_seq; }

int hookw_call(const char *json_line, hookw_done cb, void *ctx) {
  size_t n = strlen(json_line);
  if (n >= LINE_MAX_LEN) return -1;
  struct done *dn = calloc(1, sizeof *dn);
  if (!dn) return -1;
  dn->cb = cb;
  dn->ctx = ctx;
  pthread_mutex_lock(&mu);
  if (q_n == CALL_MAX) { pthread_mutex_unlock(&mu); free(dn); return -1; }
  struct call *c = &q[(q_head + q_n) % CALL_MAX];
  memcpy(c->line, json_line, n + 1);
  c->dn = dn;
  c->t0 = now_ms();
  q_n++;
  pthread_cond_signal(&cv);
  pthread_mutex_unlock(&mu);
  return 0;
}

int hookw_on_change(void (*fn)(void)) {
  if (g_n_listeners == MAX_LISTENERS) return -1;
  g_listeners[g_n_listeners++] = fn;
  return 0;
}
