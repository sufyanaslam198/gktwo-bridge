// Real AF_UNIX server under /tmp; a test thread plays the UI tick.
#include <errno.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <time.h>
#include <unistd.h>

#include "../src/chitu_abi.h"
#include "../src/log.h"
#include "../src/queue.h"
#include "../src/server.h"
#include "../../gksdcp/src/hook_io.h"  // the real client

__attribute__((visibility("hidden"))) int gk_abi_enabled = 0;
void gk_log(const char* fmt, ...) {
  va_list ap; va_start(ap, fmt); fputs("log: ", stderr); vfprintf(stderr, fmt, ap); fputc('\n', stderr); va_end(ap);
}

static int fails;
#define CHECK(c) do { if (!(c)) { fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); fails++; } } while (0)

#define SOCK "/tmp/gkhook-test.sock"
#define DIR "/tmp/gkhook-test-files"

static long now_ms(void) {
  struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t);
  return t.tv_sec * 1000 + t.tv_nsec / 1000000;
}

static int connect_sock(void) {
  struct sockaddr_un sa; memset(&sa, 0, sizeof sa);
  sa.sun_family = AF_UNIX; strcpy(sa.sun_path, SOCK);
  int fd = socket(AF_UNIX, SOCK_STREAM, 0);
  if (connect(fd, (struct sockaddr*)&sa, sizeof sa) != 0) { close(fd); return -1; }
  return fd;
}

// send line, read reply until EOF/newline; returns length (0 = nothing/closed)
static size_t rpc(const char* line, char* reply, size_t cap) {
  int fd = connect_sock();
  if (fd < 0) return 0;
  struct timeval tv = {5, 0};
  setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
  if (line) send(fd, line, strlen(line), MSG_NOSIGNAL);
  size_t n = 0;
  while (n < cap - 1) {
    ssize_t r = recv(fd, reply + n, cap - 1 - n, 0);
    if (r <= 0) break;
    n += (size_t)r;
    if (reply[n - 1] == '\n') break;
  }
  reply[n] = 0;
  close(fd);
  return n;
}

static int has(const char* line, const char* needle) {
  char r[2100];
  rpc(line, r, sizeof r);
  if (!strstr(r, needle)) fprintf(stderr, "  got: %s  want: %s\n", r, needle);
  return strstr(r, needle) != NULL;
}


// ---- concurrency helpers ----
struct rpc_job { const char* line; char reply[2100]; long ms; };
static void* rpc_thread(void* a) {
  struct rpc_job* j = a;
  long t0 = now_ms();
  rpc(j->line, j->reply, sizeof j->reply);
  j->ms = now_ms() - t0;
  return NULL;
}

// ---- fake tick ----
static volatile int tick_run = 1, tick_mode;  // 0 accept, 1 refuse "nope", 2 ignore
static volatile int starts_seen;
static char start_path[1100];
static volatile int z_seen_kind = -1, z_seen_mm, z_seen_n;
static void* tick(void* a) {
  (void)a;
  struct gk_cmd c;
  while (tick_run) {
    if (tick_mode == 2 || !gkq_take(&c)) { usleep(1000); continue; }
    if (c.kind == CMD_START) { strcpy(start_path, c.path); starts_seen++; continue; }
    if (c.kind >= CMD_Z_MOVE) { z_seen_kind = (int)c.kind; z_seen_mm = c.mm; z_seen_n++; }
    gkq_set_result(c.id, tick_mode == 0, tick_mode == 0 ? NULL : "nope");
  }
  return NULL;
}

// ---- validators ----
static volatile int full_result;  // 0 pass
static int cheap_v(const char* p, char* e, size_t n) {
  if (strstr(p, "cheapbad")) { snprintf(e, n, "cheap says no"); return -1; }
  return 0;
}
static int full_v(const char* p, char* e, size_t n) {
  (void)p;
  usleep(150000);
  if (full_result) { snprintf(e, n, "file check failed"); return -1; }
  return 0;
}

static int pwo;  // publish(): print window open flag
static int pz_moving;  // publish(): Z moving flag (z_avail follows)
static void publish(const char* state, int file) {
  struct gk_status s; memset(&s, 0, sizeof s);
  s.print_window_open = pwo;
  s.z_avail = pz_moving; s.z_homed = pz_moving; s.z_moving = pz_moving;
  snprintf(s.state, sizeof s.state, "%s", state);
  s.layer = s.total_layers = s.elapsed_s = s.remaining_s = -1; s.progress = -1;
  if (file) { s.have_file = 1; strcpy(s.file, DIR "/a.ctb"); s.layer = 3; s.total_layers = 10; s.progress = 30; s.elapsed_s = 60; s.remaining_s = 140; }
  gkq_publish_status(&s);
}

static void touch(const char* p) { FILE* f = fopen(p, "w"); if (f) { fputs("x", f); fclose(f); } }

int main(void) {
  char r[2100];
  setenv("GKHOOK_FILESDIR", DIR, 1);
  CHECK(system("rm -rf " DIR " && mkdir -p " DIR "/sub") == 0);
  touch(DIR "/a.ctb"); touch(DIR "/my file (1).ctb"); touch(DIR "/b.txt"); touch(DIR "/sub/z.ctb");
  gkq_reset();

  // stale socket: a bound-then-abandoned socket file must not stop the bind
  {
    struct sockaddr_un sa; memset(&sa, 0, sizeof sa);
    sa.sun_family = AF_UNIX; strcpy(sa.sun_path, SOCK);
    unlink(SOCK);
    int s = socket(AF_UNIX, SOCK_STREAM, 0);
    CHECK(bind(s, (struct sockaddr*)&sa, sizeof sa) == 0);
    close(s);  // file stays
    struct stat st; CHECK(stat(SOCK, &st) == 0);
  }
  gk_start_ops.cheap = cheap_v;
  gk_start_ops.full = full_v;
  CHECK(gk_server_start(SOCK) == 0);
  for (int i = 0; i < 200 && connect_sock() < 0; i++) usleep(10000);
  pthread_t t; pthread_create(&t, NULL, tick, NULL);
  {  // F9: gksdcp runs as root; the control socket must not be world-writable
    struct stat st;
    CHECK(stat(SOCK, &st) == 0 && (st.st_mode & 0777) == 0600);
  }

  // abi disabled: every command says wrong chitu version, nothing queued
  gk_abi_enabled = 0;
  publish("idle", 0);
  const char* all[] = {"{\"cmd\":\"status\"}\n", "{\"cmd\":\"start\",\"path\":\"" DIR "/a.ctb\"}\n",
                       "{\"cmd\":\"pause\"}\n", "{\"cmd\":\"resume\"}\n", "{\"cmd\":\"stop\"}\n"};
  const char* zall[] = {"{\"cmd\":\"z_move\",\"mm\":10}\n", "{\"cmd\":\"z_top\"}\n", "{\"cmd\":\"z_home\"}\n", "{\"cmd\":\"z_stop\"}\n"};
  for (int i = 0; i < 4; i++) CHECK(has(zall[i], "{\"ok\":false,\"error\":\"wrong chitu version\"}\n"));  // test_server_z_wrong_version
  CHECK(z_seen_n == 0);
  for (int i = 0; i < 5; i++) CHECK(has(all[i], "{\"ok\":false,\"error\":\"wrong chitu version\"}\n"));
  CHECK(starts_seen == 0 && !gkq_start_in_flight());

  gk_abi_enabled = 1;
  // bad requests
  CHECK(has("garbage\n", "\"ok\":false"));
  CHECK(has("{\"cmd\":\"reboot\"}\n", "\"ok\":false"));
  CHECK(has("{\"cmd\":\"start\"}\n", "\"ok\":false"));

  // status returns the published snapshot, verbatim shape
  CHECK(has("{\"cmd\":\"status\"}\n", "{\"ok\":true,\"state\":\"idle\",\"file\":null,\"layer\":null,\"total_layers\":null,\"progress\":null,\"elapsed_s\":null,\"remaining_s\":null,\"z\":null,\"phase\":null,\"screen\":null}\n"));
  publish("printing", 1);
  CHECK(has("{\"cmd\":\"status\"}\n", "\"state\":\"printing\",\"file\":\"" DIR "/a.ctb\",\"layer\":3,\"total_layers\":10,\"progress\":30.00,\"elapsed_s\":60,\"remaining_s\":140,\"z\":null,\"phase\":null,\"screen\":null}\n"));
  // request split in two writes (gksdcp does this) works too
  {
    int fd = connect_sock();
    send(fd, "{\"cmd\":", 7, 0); usleep(50000); send(fd, "\"status\"}\n", 10, 0);
    size_t n = (size_t)recv(fd, r, sizeof r - 1, 0); r[n] = 0; close(fd);
    CHECK(strstr(r, "\"state\":\"printing\"") != NULL);
  }

  // pause/resume/stop through the tick
  CHECK(has("{\"cmd\":\"pause\"}\n", "{\"ok\":true}\n"));
  CHECK(has("{\"cmd\":\"resume\"}\n", "{\"ok\":true}\n"));
  CHECK(has("{\"cmd\":\"stop\"}\n", "{\"ok\":true}\n"));
  tick_mode = 1;
  CHECK(has("{\"cmd\":\"pause\"}\n", "{\"ok\":false,\"error\":\"nope\"}\n"));
  tick_mode = 2;
  {
    long t0 = now_ms();
    CHECK(has("{\"cmd\":\"stop\"}\n", "{\"ok\":false,\"error\":\"printer not responding\"}\n"));
    long el = now_ms() - t0;
    CHECK(el >= 1900 && el < 2900);  // must answer inside gksdcp's 3 s
  }
  // concurrency 1: a status request is answered while a pause waits on a tick that never answers
  tick_mode = 2;
  {
    struct rpc_job pj = { "{\"cmd\":\"pause\"}\n", {0}, 0 };
    pthread_t pt; pthread_create(&pt, NULL, rpc_thread, &pj);
    usleep(150000);  // the pause is now waiting
    struct rpc_job sj = { "{\"cmd\":\"status\"}\n", {0}, 0 };
    rpc_thread(&sj);
    CHECK(strstr(sj.reply, "\"ok\":true") != NULL && sj.ms < 300);
    pthread_join(pt, NULL);
    CHECK(strstr(pj.reply, "printer not responding") != NULL);
  }
  tick_mode = 0;

  // concurrency 2: 8 slow clients fill the limit; more clients get "hook busy" at once; service resumes
  {
    int slow[9];
    for (int i = 0; i < 9; i++) slow[i] = connect_sock();
    usleep(150000);
    long t0 = now_ms();
    CHECK(has("{\"cmd\":\"status\"}\n", "{\"ok\":false,\"error\":\"hook busy\"}\n"));
    CHECK(now_ms() - t0 < 300);
    struct timeval tv = {2, 0};
    setsockopt(slow[8], SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
    ssize_t n = recv(slow[8], r, sizeof r - 1, 0);  // the 9th slow client was refused too
    CHECK(n > 0 && (r[n] = 0, strstr(r, "hook busy") != NULL));
    // the slow ones hit the 1 s read deadline; poll for recovery (cap 3 s)
    int back = 0;
    for (long t1 = now_ms(); !back && now_ms() - t1 < 3000; ) {
      rpc("{\"cmd\":\"status\"}\n", r, sizeof r);
      back = strstr(r, "\"ok\":true") != NULL;
      if (!back) usleep(50000);
    }
    CHECK(back);
    for (int i = 0; i < 9; i++) close(slow[i]);
  }

  // hung client (connects, says nothing): bounded, next request still served
  {
    int hung = connect_sock();
    long t0 = now_ms();
    CHECK(has("{\"cmd\":\"status\"}\n", "\"ok\":true"));
    CHECK(now_ms() - t0 < 2500);
    close(hung);
  }
  // partial request without newline, then close: dropped quickly
  {
    int fd = connect_sock();
    send(fd, "{\"cmd\":\"sta", 11, 0); close(fd);
    long t0 = now_ms();
    CHECK(has("{\"cmd\":\"status\"}\n", "\"ok\":true"));
    CHECK(now_ms() - t0 < 500);
  }
  // oversized request without newline: dropped, loop survives
  {
    int fd = connect_sock();
    char big[3000]; memset(big, 'a', sizeof big);
    send(fd, big, sizeof big, MSG_NOSIGNAL);
    ssize_t n = recv(fd, r, sizeof r, 0);
    CHECK(n <= 0);  // dropped without a reply
    close(fd);
    CHECK(has("{\"cmd\":\"status\"}\n", "\"ok\":true"));
  }
  // client that closes before reading the reply must not kill the process (MSG_NOSIGNAL)
  {
    int fd = connect_sock();
    send(fd, "{\"cmd\":\"status\"}\n", 17, 0); close(fd);
    CHECK(has("{\"cmd\":\"status\"}\n", "\"ok\":true"));
  }

  // test_server_z_forwarded: the tick sees the command and its result is replied verbatim
  CHECK(has("{\"cmd\":\"z_move\",\"mm\":-10}\n", "{\"ok\":true}\n"));
  CHECK(z_seen_n == 1 && z_seen_kind == CMD_Z_MOVE && z_seen_mm == -10);
  CHECK(has("{\"cmd\":\"z_top\"}\n", "{\"ok\":true}\n") && z_seen_kind == CMD_Z_TOP);
  CHECK(has("{\"cmd\":\"z_home\"}\n", "{\"ok\":true}\n") && z_seen_kind == CMD_Z_HOME);
  CHECK(has("{\"cmd\":\"z_stop\"}\n", "{\"ok\":true}\n") && z_seen_kind == CMD_Z_STOP && z_seen_n == 4);
  tick_mode = 1;
  CHECK(has("{\"cmd\":\"z_move\",\"mm\":1}\n", "{\"ok\":false,\"error\":\"nope\"}\n") && z_seen_mm == 1);
  tick_mode = 0;
  // malformed mm never reaches the tick
  CHECK(has("{\"cmd\":\"z_move\",\"mm\":5}\n", "{\"ok\":false,\"error\":\"bad request\"}\n"));
  CHECK(has("{\"cmd\":\"z_move\"}\n", "\"bad request\""));
  CHECK(z_seen_n == 5);

  // ---- start ----
  publish("printing", 1);
  CHECK(has("{\"cmd\":\"start\",\"path\":\"" DIR "/a.ctb\"}\n", "{\"ok\":false,\"error\":\"printer busy\"}\n"));
  // F2: an errored print window still open: refuse up front, before any validation
  pwo = 1;
  publish("error", 1);
  CHECK(has("{\"cmd\":\"start\",\"path\":\"" DIR "/a.ctb\"}\n", "{\"ok\":false,\"error\":\"dismiss the finished print on the printer first\"}\n"));
  CHECK(has("{\"cmd\":\"start\",\"path\":\"/etc/passwd\"}\n", "dismiss the finished print on the printer first"));
  CHECK(!gkq_start_in_flight() && starts_seen == 0);
  CHECK(has("{\"cmd\":\"status\"}\n", "\"state\":\"error\""));
  { char rr[2100]; rpc("{\"cmd\":\"status\"}\n", rr, sizeof rr); CHECK(!strstr(rr, "print_window")); }  // not serialized
  // a completed print window still open: the tick closes it, so the start goes on to the checks
  publish("complete", 1);
  CHECK(has("{\"cmd\":\"start\",\"path\":\"" DIR "/missing.ctb\"}\n", "file not found"));
  CHECK(has("{\"cmd\":\"start\",\"path\":\"/etc/passwd\"}\n", "file is not in the files folder"));
  pwo = 0;
  publish("complete", 1);   // dismissed: complete without the window is startable
  CHECK(has("{\"cmd\":\"start\",\"path\":\"" DIR "/missing.ctb\"}\n", "file not found"));
  publish("idle", 0);
  CHECK(has("{\"cmd\":\"start\",\"path\":\"/etc/passwd\"}\n", "\"ok\":false"));
  CHECK(has("{\"cmd\":\"start\",\"path\":\"" DIR "/../etc/a.ctb\"}\n", "\"ok\":false"));
  CHECK(has("{\"cmd\":\"start\",\"path\":\"" DIR "/sub/z.ctb\"}\n", "\"ok\":false"));
  CHECK(has("{\"cmd\":\"start\",\"path\":\"" DIR "/..\"}\n", "bad file path"));
  CHECK(has("{\"cmd\":\"start\",\"path\":\"" DIR "/.\"}\n", "bad file path"));
  CHECK(has("{\"cmd\":\"start\",\"path\":\"" DIR "/\"}\n", "bad file path"));
  CHECK(has("{\"cmd\":\"start\",\"path\":\"" DIR "/../x.ctb\"}\n", "bad file path"));
  CHECK(has("{\"cmd\":\"start\",\"path\":\"" DIR "/a..b.ctb\"}\n", "file not found"));   // a dotted name is one safe component
  CHECK(has("{\"cmd\":\"start\",\"path\":\"" DIR "/b.txt\"}\n", "\"ok\":false"));
  CHECK(has("{\"cmd\":\"start\",\"path\":\"" DIR "/missing.ctb\"}\n", "file not found"));
  CHECK(has("{\"cmd\":\"start\",\"path\":\"" DIR "/sub\"}\n", "\"ok\":false"));
  CHECK(has("{\"cmd\":\"start\",\"path\":\"" DIR "/cheapbad.ctb\"}\n", "\"ok\":false"));  // missing anyway
  touch(DIR "/cheapbad.ctb");
  CHECK(has("{\"cmd\":\"start\",\"path\":\"" DIR "/cheapbad.ctb\"}\n", "cheap says no"));
  CHECK(!gkq_start_in_flight() && starts_seen == 0);

  // test_server_start_refused_while_z_moving
  pz_moving = 1;
  publish("idle", 0);
  CHECK(has("{\"cmd\":\"status\"}\n", "\"z\":{\"pos\":0.00,\"homed\":true,\"moving\":true}"));
  CHECK(has("{\"cmd\":\"start\",\"path\":\"" DIR "/a.ctb\"}\n", "{\"ok\":false,\"error\":\"Z is moving\"}\n"));
  CHECK(!gkq_start_in_flight() && starts_seen == 0);
  pz_moving = 0;
  publish("idle", 0);

  // accepted: ok, and `starting` is visible right after the reply
  full_result = 0;
  CHECK(has("{\"cmd\":\"start\",\"path\":\"" DIR "/my file (1).ctb\"}\n", "{\"ok\":true}\n"));
  CHECK(has("{\"cmd\":\"status\"}\n", "\"state\":\"starting\""));
  CHECK(has("{\"cmd\":\"start\",\"path\":\"" DIR "/a.ctb\"}\n", "printer busy"));  // in flight
  for (int i = 0; i < 300 && !starts_seen; i++) usleep(10000);
  CHECK(starts_seen == 1 && strcmp(start_path, DIR "/my file (1).ctb") == 0);
  CHECK(has("{\"cmd\":\"status\"}\n", "\"state\":\"starting\""));  // until the tick clears it
  gkq_end_start(NULL);
  CHECK(has("{\"cmd\":\"status\"}\n", "\"state\":\"idle\""));

  // checker failure: back to the tick's state + message
  full_result = 1;
  CHECK(has("{\"cmd\":\"start\",\"path\":\"" DIR "/a.ctb\"}\n", "{\"ok\":true}\n"));
  CHECK(has("{\"cmd\":\"status\"}\n", "\"state\":\"starting\""));
  for (int i = 0; i < 300 && gkq_start_in_flight(); i++) usleep(10000);
  CHECK(!gkq_start_in_flight() && starts_seen == 1);
  CHECK(has("{\"cmd\":\"status\"}\n", "\"state\":\"idle\",\"file\":null,\"layer\":null,\"total_layers\":null,\"progress\":null,\"elapsed_s\":null,\"remaining_s\":null,\"z\":null,\"phase\":null,\"screen\":null,\"message\":\"file check failed\"}\n"));

  // concurrency 3: two simultaneous starts, exactly one is accepted
  {
    publish("idle", 0);
    full_result = 0;
    int before = starts_seen;
    struct rpc_job a = { "{\"cmd\":\"start\",\"path\":\"" DIR "/a.ctb\"}\n", {0}, 0 };
    struct rpc_job b = a;
    pthread_t ta, tb;
    pthread_create(&ta, NULL, rpc_thread, &a);
    pthread_create(&tb, NULL, rpc_thread, &b);
    pthread_join(ta, NULL); pthread_join(tb, NULL);
    int oka = strcmp(a.reply, "{\"ok\":true}\n") == 0, okb = strcmp(b.reply, "{\"ok\":true}\n") == 0;
    CHECK(oka + okb == 1);
    CHECK(strstr(oka ? b.reply : a.reply, "printer busy") != NULL);
    for (int i = 0; i < 300 && starts_seen == before; i++) usleep(10000);
    CHECK(starts_seen == before + 1);
    gkq_end_start(NULL);
  }

  // interop with gksdcp's real client
  {
    char rep[2100];
    CHECK(hook_request(SOCK, "{\"cmd\":\"status\"}", rep, sizeof rep, 3000) == 0 && strstr(rep, "\"state\":\"idle\"") != NULL);
    CHECK(hook_request(SOCK, "{\"cmd\":\"pause\"}", rep, sizeof rep, 3000) == 0 && strcmp(rep, "{\"ok\":true}") == 0);
    CHECK(hook_request(SOCK, "{\"cmd\":\"start\",\"path\":\"" DIR "/missing.ctb\"}", rep, sizeof rep, 3000) == 0 && strstr(rep, "\"ok\":false") != NULL);
  }

  tick_run = 0;
  pthread_join(t, NULL);
  unlink(SOCK);
  if (fails) return 1;
  puts("test_server OK");
  return 0;
}
