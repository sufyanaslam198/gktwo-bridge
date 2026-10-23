// hookw_call against a fake hook socket, with the real event loop running on the main thread.
#include "hookw.h"
#include "loop.h"
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <time.h>
#include <unistd.h>

#define CHECK(c) do { if (!(c)) { fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #c); exit(1); } } while (0)
#define SOCK "/tmp/gksdcp-test-hookw.sock"

static volatile int g_hang;                    // fake hook: 1 = read the request, never answer

static void *serve_one(void *arg) {
  int fd = (int)(long)arg;
  char buf[2048];
  ssize_t n = recv(fd, buf, sizeof buf, 0);
  if (n > 0) {
    if (g_hang) sleep(3);
    else {
      const char *r = strstr(buf, "\"status\"") ? "{\"ok\":true,\"state\":\"idle\"}\n" : "{\"ok\":true}\n";
      (void)!write(fd, r, strlen(r));
    }
  }
  close(fd);
  return NULL;
}

static void *accept_loop(void *arg) {
  int lfd = (int)(long)arg;
  for (;;) {
    int fd = accept(lfd, NULL, NULL);
    if (fd < 0) return NULL;
    pthread_t t;
    pthread_create(&t, NULL, serve_one, (void *)(long)fd);
    pthread_detach(t);
  }
}

static long long now_ms(void) {
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return (long long)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

static pthread_t loop_thread;
static struct { int n, ok; char err[128]; int on_loop; long long at; unsigned long seq; } res[32];
static int n_res;

static void on_done(void *ctx, int ok, const char *err) {
  int i = (int)(long)ctx;
  res[i].n++;
  res[i].ok = ok;
  snprintf(res[i].err, sizeof res[i].err, "%s", err);
  res[i].on_loop = pthread_equal(pthread_self(), loop_thread);
  res[i].at = now_ms();
  res[i].seq = hookw_call_seq();
  n_res++;
}

// Snapshot listener: the poll number of the latest snapshot it saw.
static unsigned long change_seq;
static void on_change(void) {
  struct hk_status h;
  change_seq = hookw_status(&h);
}
static void other_listener(void) {}

// Timer-driven test script, runs on the loop thread.
static int step;
static long long t_mark;
static int next_id;

static void script(void) {
  switch (step) {
    case 0:                                     // ok call, callback on the loop thread
      CHECK(hookw_call("{\"cmd\":\"pause\"}", on_done, (void *)(long)next_id++) == 0);
      step = 1;
      break;
    case 1:
      if (!res[0].n) break;
      CHECK(res[0].ok == 1 && res[0].on_loop == 1 && res[0].seq > 0);
      // the status does not change, but the poll after a command still reaches the listeners
      if (change_seq <= res[0].seq) { CHECK(now_ms() - res[0].at < 1000); break; }
      char big[1400];
      memset(big, 'x', sizeof big - 1);
      big[sizeof big - 1] = 0;
      CHECK(hookw_call(big, on_done, (void *)(long)31) == -1);   // over-long line
      g_hang = 1;                               // now the hook never answers
      t_mark = now_ms();
      CHECK(hookw_call("{\"cmd\":\"pause\"}", on_done, (void *)(long)next_id++) == 0);
      step = 2;
      break;
    case 2:
      if (!res[1].n) break;
      CHECK(res[1].ok == 0 && strcmp(res[1].err, "printer not responding") == 0 && res[1].on_loop);
      CHECK(res[1].at - t_mark >= 2000 && res[1].at - t_mark < 2500);
      t_mark = now_ms();
      CHECK(hookw_call("{\"cmd\":\"pause\"}", on_done, (void *)(long)next_id++) == 0);   // id 2: in flight
      step = 3;
      t_mark = now_ms();
      break;
    case 3:
      if (now_ms() - t_mark < 700) break;       // let the worker finish a poll and take it
      for (int i = 0; i < 16; i++) CHECK(hookw_call("{\"cmd\":\"pause\"}", on_done, (void *)(long)(3 + i)) == 0);
      CHECK(hookw_call("{\"cmd\":\"pause\"}", on_done, (void *)(long)31) == -1);   // queue full
      step = 4;
      break;
    case 4:
      if (n_res < 2 + 17) break;                // all 17 expire with the timeout error
      for (int i = 2; i < 19; i++) CHECK(res[i].n == 1 && res[i].ok == 0);
      g_hang = 0;
      raise(SIGTERM);
      step = 5;
      break;
  }
}

int main(void) {
  unlink(SOCK);
  int lfd = socket(AF_UNIX, SOCK_STREAM, 0);
  struct sockaddr_un sa = { .sun_family = AF_UNIX };
  strcpy(sa.sun_path, SOCK);
  CHECK(bind(lfd, (struct sockaddr *)&sa, sizeof sa) == 0 && listen(lfd, 64) == 0);
  pthread_t t;
  pthread_create(&t, NULL, accept_loop, (void *)(long)lfd);

  CHECK(hookw_init(SOCK) == 0);
  CHECK(hookw_on_change(on_change) == 0);
  for (int i = 0; i < 7; i++) CHECK(hookw_on_change(other_listener) == 0);   // 8 slots
  CHECK(hookw_on_change(other_listener) == -1);
  loop_thread = pthread_self();
  loop_every(20, script);
  struct gk_opts o = { .ws_port = 0 };
  CHECK(loop_run(&o) == 0);
  CHECK(step == 5);
  unlink(SOCK);
  return 0;
}
