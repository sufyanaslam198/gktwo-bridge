// Shared state between socket threads and the UI tick, behind one mutex.
#include "queue.h"

#include <errno.h>
#include <pthread.h>
#include <string.h>
#include <time.h>

#define QCAP 16
#define NSLOT 16

struct slot { int used, done, ok; uint64_t id; char err[64]; };

static pthread_mutex_t mu = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t cv;
static pthread_once_t once = PTHREAD_ONCE_INIT;

static struct gk_cmd fifo[QCAP];
static int fifo_n;
static struct slot slots[NSLOT];
static uint64_t next_id;  // never reset: stale ids stay stale
static int published;
static struct gk_status snap;
static int in_flight;
static uint32_t start_gen;
static int checker_fin;   // the checker of the current generation has finished
static char fail_msg[128];

static void init_cv(void) {
  pthread_condattr_t a;
  pthread_condattr_init(&a);
  pthread_condattr_setclock(&a, CLOCK_MONOTONIC);
  pthread_cond_init(&cv, &a);
  pthread_condattr_destroy(&a);
}

static void copy_str(char* dst, size_t n, const char* s) {
  if (!n) return;
  size_t l = s ? strnlen(s, n - 1) : 0;
  if (l) memcpy(dst, s, l);
  dst[l] = 0;
}

static int push(const struct gk_cmd* c, uint64_t id) {
  if (fifo_n >= QCAP) return 0;
  fifo[fifo_n] = *c;
  fifo[fifo_n].id = id;
  fifo_n++;
  return 1;
}

int gkq_post(const struct gk_cmd* c) {
  pthread_once(&once, init_cv);
  pthread_mutex_lock(&mu);
  int r = push(c, 0);
  pthread_mutex_unlock(&mu);
  return r;
}

int gkq_take(struct gk_cmd* out) {
  pthread_mutex_lock(&mu);
  int r = fifo_n > 0;
  if (r) {
    *out = fifo[0];
    memmove(&fifo[0], &fifo[1], (size_t)(fifo_n - 1) * sizeof fifo[0]);
    fifo_n--;
  }
  pthread_mutex_unlock(&mu);
  return r;
}

void gkq_set_result(uint64_t id, int ok, const char* err) {
  if (id == 0) return;
  pthread_once(&once, init_cv);
  pthread_mutex_lock(&mu);
  for (int i = 0; i < NSLOT; i++) {
    if (slots[i].used && !slots[i].done && slots[i].id == id) {
      slots[i].ok = ok;
      slots[i].done = 1;
      copy_str(slots[i].err, sizeof slots[i].err, err);
      pthread_cond_broadcast(&cv);
      break;
    }
  }
  pthread_mutex_unlock(&mu);  // unknown/expired id: the waiter gave up, discard
}

int gkq_post_wait(const struct gk_cmd* c, int timeout_ms, char* err, size_t errn) {
  struct slot* s = NULL;
  int ok = 0;
  pthread_once(&once, init_cv);
  pthread_mutex_lock(&mu);
  for (int i = 0; i < NSLOT && !s; i++)
    if (!slots[i].used) s = &slots[i];
  uint64_t id = ++next_id;
  if (!s || !push(c, id)) {
    pthread_mutex_unlock(&mu);
    copy_str(err, errn, "printer busy");
    return 0;
  }
  memset(s, 0, sizeof *s);
  s->used = 1;
  s->id = id;

  struct timespec dl;
  clock_gettime(CLOCK_MONOTONIC, &dl);
  dl.tv_sec += timeout_ms / 1000;
  dl.tv_nsec += (long)(timeout_ms % 1000) * 1000000L;
  if (dl.tv_nsec >= 1000000000L) { dl.tv_sec++; dl.tv_nsec -= 1000000000L; }
  while (!s->done) {
    if (pthread_cond_timedwait(&cv, &mu, &dl) == ETIMEDOUT) break;
  }
  if (s->done) {
    ok = s->ok;
    copy_str(err, errn, s->err);
    if (!ok && !err[0]) copy_str(err, errn, "command refused");
  } else {
    for (int i = 0; i < fifo_n; i++) {  // withdraw it if the tick never took it
      if (fifo[i].id == id) {
        memmove(&fifo[i], &fifo[i + 1], (size_t)(fifo_n - i - 1) * sizeof fifo[0]);
        fifo_n--;
        break;
      }
    }
    copy_str(err, errn, "printer not responding");
  }
  s->used = 0;  // a late result for this id finds no slot and is dropped
  pthread_mutex_unlock(&mu);
  return ok;
}

void gkq_publish_status(const struct gk_status* st) {
  pthread_mutex_lock(&mu);
  snap = *st;
  snap.message[0] = 0;
  published = 1;
  pthread_mutex_unlock(&mu);
}

int gkq_read_status(struct gk_status* st) {
  pthread_mutex_lock(&mu);
  int r = published;
  if (r) {
    *st = snap;
    if (in_flight) copy_str(st->state, sizeof st->state, "starting");
    copy_str(st->message, sizeof st->message, fail_msg);
  }
  pthread_mutex_unlock(&mu);
  return r;
}

uint32_t gkq_begin_start(const char* path) {
  (void)path;
  pthread_mutex_lock(&mu);
  uint32_t g = 0;
  if (!in_flight) {
    in_flight = 1;
    checker_fin = 0;
    if (++start_gen == 0) start_gen = 1;  // 0 means "busy"
    g = start_gen;
    fail_msg[0] = 0;
  }
  pthread_mutex_unlock(&mu);
  return g;
}

int gkq_end_start_gen(uint32_t gen, const char* msg) {
  pthread_mutex_lock(&mu);
  int r = in_flight && gen == start_gen;
  if (r) {
    in_flight = 0;
    copy_str(fail_msg, sizeof fail_msg, msg);
  }
  pthread_mutex_unlock(&mu);
  return r;
}

void gkq_checker_done(uint32_t gen) {
  pthread_mutex_lock(&mu);
  if (in_flight && gen == start_gen) checker_fin = 1;
  pthread_mutex_unlock(&mu);
}

int gkq_flight(uint32_t* gen, int* checker_done) {
  pthread_mutex_lock(&mu);
  int r = in_flight;
  if (r) { *gen = start_gen; *checker_done = checker_fin; }
  pthread_mutex_unlock(&mu);
  return r;
}

void gkq_clear_message(void) {
  pthread_mutex_lock(&mu);
  fail_msg[0] = 0;
  pthread_mutex_unlock(&mu);
}

void gkq_end_start(const char* msg) {
  pthread_mutex_lock(&mu);
  in_flight = 0;
  copy_str(fail_msg, sizeof fail_msg, msg);
  pthread_mutex_unlock(&mu);
}

int gkq_start_in_flight(void) {
  pthread_mutex_lock(&mu);
  int r = in_flight;
  pthread_mutex_unlock(&mu);
  return r;
}

uint32_t gkq_start_gen(void) {
  pthread_mutex_lock(&mu);
  uint32_t g = start_gen;
  pthread_mutex_unlock(&mu);
  return g;
}

int gkq_start_valid(uint32_t gen) {
  pthread_mutex_lock(&mu);
  int r = in_flight && gen == start_gen;
  pthread_mutex_unlock(&mu);
  return r;
}

void gkq_reset(void) {
  pthread_mutex_lock(&mu);
  fifo_n = 0;
  memset(slots, 0, sizeof slots);
  published = 0;
  in_flight = 0;
  checker_fin = 0;
  fail_msg[0] = 0;
  pthread_mutex_unlock(&mu);
}
