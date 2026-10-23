#include <math.h>
#include <stdio.h>
#include <string.h>
#include "mock_chitu.h"
#include "../src/chitu_abi.h"
#include "../src/proto.h"
#include "../src/queue.h"
#include "../src/zjog.h"

static int fails;
#define CHECK(c) do { if (!(c)) { fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); fails++; } } while (0)

static struct gk_zview V;  // default: main window, no printer
static void reset(void) {
  mock_reset();
  memset(&V, 0, sizeof V);
  V.cur = V.next = 3;
  gk_z_enabled = 1;
}
static char err[64];
static int run(enum gk_cmd_kind k, int mm) {
  struct gk_cmd c;
  memset(&c, 0, sizeof c);
  c.kind = k;
  c.mm = mm;
  err[0] = 0;
  return gk_zjog_cmd(&c, &V, err, sizeof err);
}
static int is_move(int i, int flags, float mm, float sp) {
  return M.ax[i].kind == AX_MOVE && M.ax[i].axis == 0 && M.ax[i].flags == flags &&
         M.ax[i].mm == mm && M.ax[i].speed == sp;
}
// refused with this message and no axis call at all
static int refused(int ok, const char* msg) { return !ok && strcmp(err, msg) == 0 && M.n_axis == 0; }

static void test_zjog_moves_relative(void) {
  reset();
  CHECK(run(CMD_Z_MOVE, 10) == 1 && M.n_axis == 1 && is_move(0, 1, 10.0f, 5.0f));
  reset();
  CHECK(run(CMD_Z_MOVE, -1) == 1 && M.n_axis == 1 && is_move(0, 1, -1.0f, 5.0f));
}

static void test_zjog_clamps_bottom(void) {
  reset(); M.ax_pos = 3.0f;
  CHECK(run(CMD_Z_MOVE, -10) == 1 && M.n_axis == 1 && is_move(0, 1, -3.0f, 5.0f));
  reset(); M.ax_pos = 0.0f;
  CHECK(refused(run(CMD_Z_MOVE, -1), "at bottom limit"));
  reset(); M.ax_pos = 0.005f;  // inside 0.01 mm of the limit
  CHECK(refused(run(CMD_Z_MOVE, -1), "at bottom limit"));
}

static void test_zjog_clamps_top(void) {
  reset(); M.ax_pos = 145.0f;
  CHECK(run(CMD_Z_MOVE, 10) == 1 && M.n_axis == 1 && is_move(0, 1, 5.0f, 5.0f));
  reset(); M.ax_pos = 150.0f;
  CHECK(refused(run(CMD_Z_MOVE, 1), "at top limit"));
  reset(); M.ax_pos = 151.0f;  // beyond travel: never move further up
  CHECK(refused(run(CMD_Z_MOVE, 1), "at top limit"));
}

static void test_zjog_top_and_home(void) {
  reset();
  CHECK(run(CMD_Z_TOP, 0) == 1 && M.n_axis == 1 && is_move(0, 0, 150.0f, 5.0f));
  reset(); M.ax_pos = 150.0f;
  CHECK(refused(run(CMD_Z_TOP, 0), "at top limit"));
  reset(); M.ax_homed = 0;  // unhomed is fine for home
  CHECK(run(CMD_Z_HOME, 0) == 1 && M.n_axis == 2 && M.ax[0].kind == AX_HALT && M.ax[0].axis == 0 &&
        M.ax[1].kind == AX_HOME && M.ax[1].axis == 0);
}

static void test_zjog_requires_home(void) {
  reset(); M.ax_homed = 0;
  CHECK(refused(run(CMD_Z_MOVE, 1), "home Z first"));
  CHECK(refused(run(CMD_Z_MOVE, -1), "home Z first"));
  CHECK(refused(run(CMD_Z_TOP, 0), "home Z first"));
  struct gk_status s;
  memset(&s, 0, sizeof s);
  gk_zjog_status(&s);
  CHECK(s.z_avail == 1 && s.z_homed == 0 && s.z_moving == 0);
  char buf[2048];
  CHECK(proto_reply_status(buf, sizeof buf, &s) > 0 && strstr(buf, "\"z\":{\"pos\":null,\"homed\":false") != NULL);
  reset();
  memset(&s, 0, sizeof s);
  gk_zjog_status(&s);
  CHECK(s.z_homed == 1 && s.z_pos == 50.0);
}

static void test_zjog_refuses_while_moving(void) {
  reset(); M.ax_idle = 0;
  CHECK(refused(run(CMD_Z_MOVE, 1), "Z is moving"));
  CHECK(refused(run(CMD_Z_TOP, 0), "Z is moving"));
  CHECK(refused(run(CMD_Z_HOME, 0), "Z is moving"));
  CHECK(run(CMD_Z_STOP, 0) == 1 && M.n_axis == 1 && M.ax[0].kind == AX_HALT && M.ax[0].axis == 0);
  struct gk_status s;
  memset(&s, 0, sizeof s);
  gk_zjog_status(&s);
  CHECK(s.z_moving == 1);
}

static void all_refused_n(const char* msg, int n) {
  enum gk_cmd_kind ks[] = {CMD_Z_MOVE, CMD_Z_TOP, CMD_Z_HOME, CMD_Z_STOP};
  for (int i = 0; i < n; i++) {
    CHECK(refused(run(ks[i], ks[i] == CMD_Z_MOVE ? 1 : 0), msg));
    M.n_axis = 0;
  }
}
static void all_refused(const char* msg) { all_refused_n(msg, 4); }
static void test_zjog_refuses_while_printing(void) {
  reset(); V.printer = 1; all_refused("printer busy");
  reset(); V.cur = V.next = 16; all_refused("printer busy");
  reset(); V.cur = 3; V.next = 16; all_refused("printer busy");
  reset(); V.cur = 16; V.next = 3; all_refused("printer busy");
  reset(); V.start_in_flight = 1; all_refused("printer busy");
  // window 6: every z command, including z_stop, is refused
  reset(); V.cur = 6; V.next = 6; all_refused("firmware update in progress");
  reset(); V.next = 6; all_refused("firmware update in progress");
  reset(); V.cur = 6; V.next = 3; all_refused("firmware update in progress");
}

static int moves_sane(void) {
  for (int i = 0; i < M.n_axis && i < 16; i++) {
    if (M.ax[i].kind != AX_MOVE) continue;
    if (!isfinite(M.ax[i].mm) || fabsf(M.ax[i].mm) > 300.0f) return 0;
    if (!isfinite(M.ax[i].speed) || M.ax[i].speed <= 0 || M.ax[i].speed > 30.0f) return 0;
  }
  return 1;
}
static void test_zjog_insane_params(void) {
  float travels[] = {0.0f, NAN, 1e9f, -5.0f, INFINITY};
  for (int i = 0; i < 5; i++) {
    reset(); mock_axis_params(travels[i], 5.0f);
    CHECK(refused(run(CMD_Z_MOVE, 1), "Z travel limit unknown"));
    CHECK(refused(run(CMD_Z_TOP, 0), "Z travel limit unknown"));
    CHECK(run(CMD_Z_MOVE, -1) == 1 && M.n_axis == 1 && is_move(0, 1, -1.0f, 5.0f));
    CHECK(moves_sane());
    reset(); mock_axis_params(travels[i], 5.0f);
    CHECK(run(CMD_Z_HOME, 0) == 1 && M.n_axis == 2);  // home needs no travel
  }
  float speeds[] = {0.0f, NAN, 1000.0f, -1.0f, INFINITY};
  for (int i = 0; i < 5; i++) {
    reset(); mock_axis_params(150.0f, speeds[i]);
    CHECK(refused(run(CMD_Z_MOVE, 1), "Z speed setting invalid"));
    CHECK(refused(run(CMD_Z_MOVE, -1), "Z speed setting invalid"));
    CHECK(refused(run(CMD_Z_TOP, 0), "Z speed setting invalid"));
    CHECK(refused(run(CMD_Z_HOME, 0), "Z speed setting invalid"));
    CHECK(moves_sane());
  }
  reset(); M.ax_pos = NAN;  // garbage position: never a non-finite delta
  CHECK(M.n_axis == 0);
  run(CMD_Z_MOVE, 1); run(CMD_Z_MOVE, -1); run(CMD_Z_TOP, 0);
  CHECK(M.n_axis == 0);
}

static void test_zjog_unavailable(void) {
  reset(); gk_z_enabled = 0;
  all_refused("Z control unavailable");
  struct gk_status s;
  memset(&s, 0, sizeof s);
  gk_zjog_status(&s);
  CHECK(s.z_avail == 0);
  CHECK(M.n_axis == 0);
}

int main(void) {
  test_zjog_moves_relative();
  test_zjog_clamps_bottom();
  test_zjog_clamps_top();
  test_zjog_top_and_home();
  test_zjog_requires_home();
  test_zjog_refuses_while_moving();
  test_zjog_refuses_while_printing();
  test_zjog_insane_params();
  test_zjog_unavailable();
  if (fails) { fprintf(stderr, "%d failure(s)\n", fails); return 1; }
  puts("test_zjog: ok");
  return 0;
}
