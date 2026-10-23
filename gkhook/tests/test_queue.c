#include <pthread.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include "../src/queue.h"

static int fails;
#define CHECK(c) do { if (!(c)) { fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); fails++; } } while (0)

static long ms_since(const struct timespec* a) {
  struct timespec b; clock_gettime(CLOCK_MONOTONIC, &b);
  return (b.tv_sec - a->tv_sec) * 1000 + (b.tv_nsec - a->tv_nsec) / 1000000;
}

// plays the tick: takes one command and answers it
static void* tick_one(void* arg) {
  const char* err = arg;  // NULL = accept
  struct gk_cmd c;
  for (int i = 0; i < 500; i++) {
    if (gkq_take(&c)) { gkq_set_result(c.id, err == NULL, err); return NULL; }
    usleep(2000);
  }
  return NULL;
}

static void* tick_late(void* arg) {
  (void)arg;
  struct gk_cmd c;
  for (int i = 0; i < 500; i++) {
    if (gkq_take(&c)) { usleep(200000); gkq_set_result(c.id, 1, NULL); return NULL; }
    usleep(1000);
  }
  return NULL;
}

int main(void) {
  struct gk_cmd c, o;
  char err[64];

  // FIFO order, no-wait post
  gkq_reset();
  memset(&c, 0, sizeof c);
  c.kind = CMD_START; strcpy(c.path, "/media/emmc/a.ctb");
  CHECK(gkq_post(&c) == 1);
  c.kind = CMD_STOP; c.path[0] = 0;
  CHECK(gkq_post(&c) == 1);
  CHECK(gkq_take(&o) == 1 && o.kind == CMD_START && strcmp(o.path, "/media/emmc/a.ctb") == 0 && o.id == 0);
  CHECK(gkq_take(&o) == 1 && o.kind == CMD_STOP);
  CHECK(gkq_take(&o) == 0);

  // status snapshot: nothing published yet
  struct gk_status st, rd;
  memset(&st, 0, sizeof st);
  CHECK(gkq_read_status(&rd) == 0);
  strcpy(st.state, "printing"); st.layer = 5; st.total_layers = 9; st.progress = 55; st.have_file = 1;
  strcpy(st.file, "/media/emmc/a.ctb");
  gkq_publish_status(&st);
  memset(&rd, 0, sizeof rd);
  CHECK(gkq_read_status(&rd) == 1 && strcmp(rd.state, "printing") == 0 && rd.layer == 5 && strcmp(rd.file, st.file) == 0);
  CHECK(rd.message[0] == 0);

  // start in flight overlays state; failure message sticks until the next start
  CHECK(gkq_start_in_flight() == 0);
  CHECK(gkq_begin_start("/media/emmc/a.ctb") != 0);
  CHECK(gkq_begin_start("/media/emmc/b.ctb") == 0);  // atomic busy
  CHECK(gkq_start_in_flight() == 1);
  CHECK(gkq_read_status(&rd) == 1 && strcmp(rd.state, "starting") == 0);
  gkq_end_start("file check failed");
  CHECK(gkq_start_in_flight() == 0);
  CHECK(gkq_read_status(&rd) == 1 && strcmp(rd.state, "printing") == 0 && strcmp(rd.message, "file check failed") == 0);
  CHECK(gkq_begin_start("/media/emmc/c.ctb") != 0);   // clears the message
  CHECK(gkq_read_status(&rd) == 1 && rd.message[0] == 0);
  gkq_end_start(NULL);
  CHECK(gkq_read_status(&rd) == 1 && rd.message[0] == 0 && strcmp(rd.state, "printing") == 0);

  // result round trip: accepted
  gkq_reset();
  pthread_t t;
  memset(&c, 0, sizeof c); c.kind = CMD_PAUSE;
  pthread_create(&t, NULL, tick_one, NULL);
  CHECK(gkq_post_wait(&c, 2000, err, sizeof err) == 1);
  pthread_join(t, NULL);
  // refused with message
  c.kind = CMD_RESUME;
  pthread_create(&t, NULL, tick_one, (void*)"not paused");
  err[0] = 0;
  CHECK(gkq_post_wait(&c, 2000, err, sizeof err) == 0 && strcmp(err, "not paused") == 0);
  pthread_join(t, NULL);

  // no tick: times out with an error, and the abandoned command is withdrawn
  struct timespec t0; clock_gettime(CLOCK_MONOTONIC, &t0);
  c.kind = CMD_STOP;
  CHECK(gkq_post_wait(&c, 200, err, sizeof err) == 0);
  long el = ms_since(&t0);
  CHECK(el >= 190 && el < 1000);
  CHECK(strcmp(err, "printer not responding") == 0);
  CHECK(gkq_take(&o) == 0);

  // a command already taken when the waiter times out: the late result is discarded
  {
    struct gk_cmd x;
    c.kind = CMD_PAUSE;
    CHECK(gkq_post_wait(&c, 30, err, sizeof err) == 0);   // times out; withdrawn
    gkq_set_result(1, 0, "late");                          // stale id: ignored
    pthread_create(&t, NULL, tick_one, NULL);
    CHECK(gkq_post_wait(&c, 2000, err, sizeof err) == 1);  // gets its own result, not "late"
    pthread_join(t, NULL);
    CHECK(gkq_take(&x) == 0);
  }

  // taken by the tick but answered after the waiter gave up
  pthread_create(&t, NULL, tick_late, NULL);
  CHECK(gkq_post_wait(&c, 50, err, sizeof err) == 0 && strcmp(err, "printer not responding") == 0);
  pthread_join(t, NULL);                                    // late result delivered here, discarded
  pthread_create(&t, NULL, tick_one, (void*)"nope");
  CHECK(gkq_post_wait(&c, 2000, err, sizeof err) == 0 && strcmp(err, "nope") == 0);
  pthread_join(t, NULL);

  // queue full
  gkq_reset();
  int ok = 0;
  for (int i = 0; i < 100; i++) ok += gkq_post(&c);
  CHECK(ok > 0 && ok < 100);

  // start generations: a stale generation is never valid
  gkq_reset();
  CHECK(gkq_begin_start("a") != 0);
  uint32_t g1 = gkq_start_gen();
  CHECK(gkq_start_valid(g1));
  gkq_end_start(NULL);
  CHECK(!gkq_start_valid(g1));
  CHECK(gkq_begin_start("b") != 0);
  CHECK(gkq_start_gen() != g1 && !gkq_start_valid(g1) && gkq_start_valid(gkq_start_gen()));
  gkq_reset();

  // F1: begin returns the generation; endings are generation-keyed; checker-done is tracked
  {
    uint32_t ga = gkq_begin_start("a"), gg = 0;
    memset(&st, 0, sizeof st);
    gkq_publish_status(&st);
    int cd = -1;
    CHECK(ga != 0 && gkq_begin_start("b") == 0);
    CHECK(gkq_flight(&gg, &cd) == 1 && gg == ga && cd == 0);
    gkq_checker_done(ga + 1);                        // not ours: ignored
    CHECK(gkq_flight(&gg, &cd) == 1 && cd == 0);
    gkq_checker_done(ga);
    CHECK(gkq_flight(&gg, &cd) == 1 && cd == 1);
    CHECK(gkq_end_start_gen(ga + 1, "x") == 0 && gkq_start_in_flight());
    CHECK(gkq_end_start_gen(ga, "boom") == 1 && !gkq_start_in_flight());
    CHECK(gkq_flight(&gg, &cd) == 0);
    CHECK(gkq_read_status(&rd) == 1 && strcmp(rd.message, "boom") == 0);
    // a late-finishing stale checker must not end (or flag done) a newer start
    uint32_t gb = gkq_begin_start("b");
    CHECK(gb != 0 && gb != ga);
    CHECK(gkq_end_start_gen(ga, "late") == 0 && gkq_start_in_flight());
    gkq_checker_done(ga);
    CHECK(gkq_flight(&gg, &cd) == 1 && gg == gb && cd == 0);
    CHECK(gkq_read_status(&rd) == 1 && rd.message[0] == 0);
    CHECK(gkq_end_start_gen(gb, NULL) == 1 && !gkq_start_in_flight());
    // gkq_clear_message drops a lingering failure message
    CHECK(gkq_end_start_gen(gkq_begin_start("c"), "bad") == 1);
    CHECK(gkq_read_status(&rd) == 1 && strcmp(rd.message, "bad") == 0);
    gkq_clear_message();
    CHECK(gkq_read_status(&rd) == 1 && rd.message[0] == 0);
  }
  gkq_reset();

  if (fails) return 1;
  puts("test_queue OK");
  return 0;
}
