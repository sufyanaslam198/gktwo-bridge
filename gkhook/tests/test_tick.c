#include <math.h>
#include <pthread.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include "mock_chitu.h"
#include "../src/chitu_abi.h"
#include "../src/queue.h"
#include "../src/tick.h"
#include "../src/zjog.h"

static int fails;
#define CHECK(c) do { if (!(c)) { fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); fails++; } } while (0)

// ---- ordering probe (linked with --wrap): was a snapshot published before gkq_end_start? ----
static int pub_count, pub_at_end = -1;
extern void __real_gkq_publish_status(const struct gk_status*);
extern void __real_gkq_end_start(const char*);
void __wrap_gkq_publish_status(const struct gk_status* s) { pub_count++; __real_gkq_publish_status(s); }
void __wrap_gkq_end_start(const char* m) { pub_at_end = pub_count; __real_gkq_end_start(m); }

static int64_t fake_now;
static int64_t clk(void) { return fake_now; }

static char path[64];
static const unsigned char ctb6[] = {0x06, 0x01, 0xfd, 0x12, 0, 0, 0, 0};

static void reset(void) {
  mock_reset();
  gkq_reset();
  gk_tick_reset();
  gk_tick_clock_ms = clk;
  fake_now = 1000;
  pub_count = 0;
  pub_at_end = -1;
}
static void mkfile(void) { FILE* f = fopen(path, "wb"); fwrite(ctb6, 1, sizeof ctb6, f); fclose(f); }
static struct gk_status status(void) { struct gk_status s; memset(&s, 0, sizeof s); gkq_read_status(&s); return s; }
static void tick(void) { gk_tick(); }

static uint32_t post_magic = 0x12fd0106u;
static void post_start(const char* p) {
  struct gk_cmd c;
  memset(&c, 0, sizeof c);
  c.kind = CMD_START;
  snprintf(c.path, sizeof c.path, "%s", p);
  c.gen = gkq_start_gen();
  c.magic = post_magic;
  gkq_post(&c);
}
// begin a start the way the server does, then post it as the checker would
static void accept_start(const char* p) { CHECK(gkq_begin_start(p) != 0); post_start(p); }

// put the mock into "printer exists in window 16 at state s"
static void enter_print(int state) {
  mock_set_window(3, 16);
  mock_loop();
  M.state = state;
}

struct wait_arg { enum gk_cmd_kind k; int ok; char err[64]; };
static void* waiter(void* a) {
  struct wait_arg* w = a;
  struct gk_cmd c;
  memset(&c, 0, sizeof c);
  c.kind = w->k;
  w->ok = gkq_post_wait(&c, 2000, w->err, sizeof w->err);
  return NULL;
}
// issue pause/resume/stop from a "socket" thread and run one tick to answer it
static int ctl(enum gk_cmd_kind k, char* err) {
  struct wait_arg w;
  memset(&w, 0, sizeof w);
  w.k = k;
  pthread_t t;
  pthread_create(&t, NULL, waiter, &w);
  usleep(30000);  // let the command reach the queue
  tick();
  pthread_join(t, NULL);
  strcpy(err, w.err);
  return w.ok;
}

struct zwait_arg { enum gk_cmd_kind k; int mm; int ok; char err[64]; };
static void* zwaiter(void* a) {
  struct zwait_arg* w = a;
  struct gk_cmd c;
  memset(&c, 0, sizeof c);
  c.kind = w->k;
  c.mm = w->mm;
  w->ok = gkq_post_wait(&c, 2000, w->err, sizeof w->err);
  return NULL;
}
// a z command from a "socket" thread, answered by one tick
static int zctl(enum gk_cmd_kind k, int mm, char* e) {
  struct zwait_arg w;
  memset(&w, 0, sizeof w);
  w.k = k; w.mm = mm;
  pthread_t t;
  pthread_create(&t, NULL, zwaiter, &w);
  usleep(30000);
  tick();
  pthread_join(t, NULL);
  strcpy(e, w.err);
  return w.ok;
}

static void test_tick_routes_z_and_reports(void) {
  char e[64];
  struct gk_status s;
  reset(); gk_z_enabled = 1;
  tick();
  s = status();
  CHECK(s.z_avail == 1 && s.z_homed == 1 && s.z_moving == 0 && s.z_pos == 50.0);
  int pc = pub_count;
  CHECK(zctl(CMD_Z_MOVE, 10, e) == 1 && M.n_axis == 1 && M.ax[0].kind == AX_MOVE &&
        M.ax[0].flags == 1 && M.ax[0].mm == 10.0f && M.ax[0].speed == 5.0f);
  CHECK(pub_count == pc + 1);  // published right after the accepted command
  M.n_axis = 0;
  CHECK(zctl(CMD_Z_MOVE, 1, e) == 1 && M.n_axis == 1);  // a second command gets its own result
  M.ax_idle = 0;  // the axis is now moving: refused, no axis call, snapshot says moving
  M.n_axis = 0;
  CHECK(zctl(CMD_Z_TOP, 0, e) == 0 && !strcmp(e, "Z is moving") && M.n_axis == 0);
  s = status();
  CHECK(s.z_moving == 1);
  // while printing: refused, never reaches the axis
  reset(); gk_z_enabled = 1; enter_print(4);
  CHECK(zctl(CMD_Z_STOP, 0, e) == 0 && !strcmp(e, "printer busy") && M.n_axis == 0);
  // unavailable: refused and the snapshot carries z_avail 0
  reset(); gk_z_enabled = 0;
  CHECK(zctl(CMD_Z_HOME, 0, e) == 0 && !strcmp(e, "Z control unavailable") && M.n_axis == 0);
  s = status(); CHECK(s.z_avail == 0);
  gk_z_enabled = 0;
}

static void test_tick_start_refused_while_z_moving(void) {
  struct gk_status s;
  reset(); gk_z_enabled = 1; M.ax_idle = 0;
  accept_start(path); tick();
  s = status();
  CHECK(M.n_setwin == 0 && !gkq_start_in_flight() && !strcmp(s.message, "Z is moving"));
  reset(); gk_z_enabled = 1;  // idle axis: start proceeds
  accept_start(path); tick();
  CHECK(M.n_setwin == 1);
  reset(); gk_z_enabled = 0; M.ax_idle = 0;  // disabled: the axis is never consulted
  accept_start(path); tick();
  CHECK(M.n_setwin == 1);
  gk_z_enabled = 0;
}

// a start and z commands in the same drain: the start's pending window switch blocks them all
static void test_tick_start_then_z_same_drain(void) {
  enum gk_cmd_kind ks[] = {CMD_Z_STOP, CMD_Z_MOVE};
  for (int i = 0; i < 2; i++) {
    reset(); gk_z_enabled = 1;
    accept_start(path);
    struct zwait_arg w;
    memset(&w, 0, sizeof w);
    w.k = ks[i]; w.mm = 1;
    pthread_t t;
    pthread_create(&t, NULL, zwaiter, &w);
    usleep(30000);  // the z command is queued behind the start
    tick();
    pthread_join(t, NULL);
    CHECK(M.n_setwin == 1 && M.n_axis == 0 && w.ok == 0 && !strcmp(w.err, "printer busy"));
  }
  gk_z_enabled = 0;
}

// A finished print still on screen: the start first closes it as the dialog's tick button does
// (close the dialog, window 3), then starts as usual. Other dialogs and an error are left alone.
static void test_tick_start_dismisses_finished_print(void) {
  char err[64];
  struct gk_status s;
  // complete dialog open: closed, window 3, then window 16 once the print window is gone
  reset(); enter_print(2); mock_dialog(GK_MSGBOX_PRINT_COMPLETE, 1); tick();
  accept_start(path); tick();
  CHECK(M.n_msgbox_close == 1 && M.n_setwin == 1 && M.win_idx[0] == 3 && M.win_param[0] == NULL);
  CHECK(gkq_start_in_flight());
  s = status(); CHECK(!strcmp(s.state, "starting") && !s.message[0]);
  tick();
  CHECK(M.n_setwin == 1);              // waits for the window switch
  mock_loop();
  CHECK(M.left_with_dialog == 0 && mock_printer() == NULL && mock_ui_current() == 3);
  tick();
  CHECK(M.n_setwin == 2 && M.win_idx[1] == 16 && M.win_param[1] == (void*)mock_mem(GK_STATIC_ITEM));
  mock_loop(); M.state = 3; tick();
  CHECK(!gkq_start_in_flight());
  s = status(); CHECK(!strcmp(s.state, "printing") && !s.message[0]);
  CHECK(M.n_msgbox_close == 1);

  // dialog already closed on the touchscreen (window 16 still open): window 3 only
  reset(); enter_print(2); tick();
  accept_start(path); tick();
  CHECK(M.n_msgbox_close == 0 && M.n_setwin == 1 && M.win_idx[0] == 3 && gkq_start_in_flight());
  mock_loop(); tick();
  CHECK(M.n_setwin == 2 && M.win_idx[1] == 16);

  // dialog queued but not on screen yet: wait for app_msgbox_run to show it, then close it
  reset(); enter_print(2); mock_dialog(GK_MSGBOX_PRINT_COMPLETE, 0);
  accept_start(path); tick();
  CHECK(M.n_msgbox_close == 0 && M.n_setwin == 0 && gkq_start_in_flight());
  CHECK(*mock_mem(GK_MSGBOX_BUSY) == 1);   // shown by the chained app_update
  tick();
  CHECK(M.n_msgbox_close == 1 && M.n_setwin == 1 && M.win_idx[0] == 3);
  mock_loop(); CHECK(M.left_with_dialog == 0);

  // another dialog open over the finished print: refused, nothing touched
  reset(); enter_print(2); mock_dialog(0x00015ae0u, 1);
  accept_start(path); tick();
  CHECK(M.n_msgbox_close == 0 && M.n_setwin == 0 && !gkq_start_in_flight());
  s = status(); CHECK(!strcmp(s.message, "dismiss the finished print on the printer first"));

  // error: never closed remotely
  reset(); enter_print(9); mock_dialog(0x0001cda8u, 1);
  accept_start(path); tick();
  CHECK(M.n_msgbox_close == 0 && M.n_setwin == 0 && !gkq_start_in_flight());
  // printer at state 0 in window 16 (heat dialog): untouched
  reset(); enter_print(0); mock_dialog(0x0001c8b8u, 1);
  accept_start(path); tick();
  CHECK(M.n_msgbox_close == 0 && M.n_setwin == 0 && !gkq_start_in_flight());

  // stop while waiting for the dialog: cancelled, nothing touched
  reset(); enter_print(2); mock_dialog(GK_MSGBOX_PRINT_COMPLETE, 0);
  accept_start(path); tick();
  CHECK(ctl(CMD_STOP, err) == 1);
  CHECK(!gkq_start_in_flight() && M.n_stop == 0 && M.n_msgbox_close == 0 && M.n_setwin == 0);
  s = status(); CHECK(!strcmp(s.message, "start cancelled"));
  // stop after the dismissal: the print window still closes, the print does not start
  reset(); enter_print(2); mock_dialog(GK_MSGBOX_PRINT_COMPLETE, 1);
  accept_start(path); tick();
  CHECK(ctl(CMD_STOP, err) == 1);
  CHECK(!gkq_start_in_flight() && M.n_stop == 0);
  s = status(); CHECK(!strcmp(s.message, "start cancelled"));
  mock_loop(); tick(); tick();
  CHECK(M.n_setwin == 1 && mock_ui_current() == 3 && M.left_with_dialog == 0);

  // the print window never closes: the start fails after the window wait
  reset(); enter_print(2);
  accept_start(path); tick();
  CHECK(M.n_setwin == 1 && gkq_start_in_flight());
  fake_now += 4000; tick();
  CHECK(gkq_start_in_flight());
  fake_now += 2000; tick();
  CHECK(!gkq_start_in_flight() && M.n_setwin == 1);
  s = status(); CHECK(!strcmp(s.message, "start failed"));

  // a start whose generation ended meanwhile is dropped after the dismissal
  reset(); enter_print(2);
  accept_start(path); tick();
  gkq_end_start("file check timed out");
  mock_loop(); tick(); tick();
  CHECK(M.n_setwin == 1 && !gkq_start_in_flight());
}

int main(void) {
  char err[64];
  struct gk_status s;
  int before;
  snprintf(path, sizeof path, "/tmp/gkt_tick_%d.ctb", (int)getpid());
  mkfile();

  // ---- status mapping ----
  reset();
  tick();
  s = status();
  CHECK(!strcmp(s.state, "idle") && !s.have_file && s.layer == -1 && s.total_layers == -1 &&
        s.progress < 0 && s.elapsed_s == -1 && s.remaining_s == -1);
  CHECK(M.n_app_update == 1);

  struct { int st; const char* name; } map[] = {
    {1, "printing"}, {2, "complete"}, {3, "printing"}, {4, "printing"}, {5, "printing"},
    {6, "printing"}, {7, "printing"}, {8, "paused"}, {9, "error"}, {0, "starting"} };
  for (unsigned i = 0; i < sizeof map / sizeof map[0]; i++) {
    reset();
    enter_print(map[i].st);
    M.layer = 10; M.print_tick = 5000; M.total_tick = 65000;
    tick();
    s = status();
    CHECK(!strcmp(s.state, map[i].name));
    CHECK(s.layer == 10 && s.total_layers == 100 && s.elapsed_s == 5 && s.remaining_s == 60);
    CHECK(fabs(s.progress - 10.0) < 0.001);
  }
  // phase from the printer state; "" (null) when idle, complete, suspended, error or no printer
  {
    struct { int st; const char* phase; } pm[] = {
      {0, ""}, {1, "homing"}, {2, ""}, {3, "exposing"}, {4, "exposing"}, {5, "exposing"},
      {6, "pausing"}, {7, "stopping"}, {8, ""}, {9, ""} };
    for (unsigned i = 0; i < sizeof pm / sizeof pm[0]; i++) {
      reset(); enter_print(pm[i].st); tick(); s = status();
      CHECK(!strcmp(s.phase, pm[i].phase));
    }
    reset(); tick(); s = status();
    CHECK(!s.phase[0]);
  }
  // screen resolution from chitu's globals, with or without a printer; bad values give 0, 0
  {
    reset(); *(uint16_t*)mock_mem(GK_RES_X) = 7680; *(uint16_t*)mock_mem(GK_RES_Y) = 4320;
    tick(); s = status();
    CHECK(s.screen_x == 7680 && s.screen_y == 4320);
    reset(); *(uint16_t*)mock_mem(GK_RES_X) = 7680; *(uint16_t*)mock_mem(GK_RES_Y) = 4320;
    enter_print(4); tick(); s = status();
    CHECK(s.screen_x == 7680 && s.screen_y == 4320);
    uint16_t bad[][2] = { {0, 4320}, {7680, 0}, {20001, 4320}, {7680, 20001}, {0, 0} };
    for (unsigned i = 0; i < sizeof bad / sizeof bad[0]; i++) {
      reset(); *(uint16_t*)mock_mem(GK_RES_X) = bad[i][0]; *(uint16_t*)mock_mem(GK_RES_Y) = bad[i][1];
      tick(); s = status();
      CHECK(s.screen_x == 0 && s.screen_y == 0);
    }
  }
  // no total estimate / no layers: unknown, no NaN
  reset(); enter_print(4); M.total_layers = 0; M.total_tick = 0; M.print_tick = 1000;
  tick(); s = status();
  CHECK(s.total_layers == -1 && s.progress < 0 && !isnan(s.progress) && s.remaining_s == -1 && s.elapsed_s == 1);
  // no file handle: get_file_info (no NULL check in chitu) is never called
  reset(); M.has_file = 0; enter_print(4); tick(); s = status();
  CHECK(M.n_info == 0 && s.total_layers == -1 && !strcmp(s.state, "printing"));
  // elapsed beyond estimate, layer beyond total: clamped
  reset(); enter_print(4); M.layer = 500; M.print_tick = 90000; M.total_tick = 60000;
  tick(); s = status();
  CHECK(s.progress == 100 && s.remaining_s == 0);
  // path of the running job is published
  reset(); strcpy((char*)mock_mem(GK_STATIC_ITEM) + 0x100, "/media/emmc/z.ctb"); enter_print(4); tick(); s = status();
  CHECK(s.have_file && !strcmp(s.file, "/media/emmc/z.ctb"));

  // ---- command guards ----
  reset(); enter_print(3);
  accept_start(path); tick();   // printing: start refused
  CHECK(M.n_setwin == 0 && !gkq_start_in_flight());
  s = status(); CHECK(!strcmp(s.message, "printer busy"));

  reset();   // idle
  CHECK(ctl(CMD_PAUSE, err) == 0 && !strcmp(err, "not printing"));
  CHECK(ctl(CMD_STOP, err) == 0 && !strcmp(err, "not printing") && M.n_stop == 0);
  CHECK(ctl(CMD_RESUME, err) == 0);
  CHECK(M.n_pause == 0 && M.n_resume == 0);

  reset(); enter_print(1);   // homing: pause has its own message
  CHECK(ctl(CMD_PAUSE, err) == 0 && !strcmp(err, "printer is homing - pause when printing starts") && M.n_pause == 0);
  reset(); enter_print(4);
  CHECK(ctl(CMD_PAUSE, err) == 1 && M.n_pause == 1);
  reset(); enter_print(2);
  CHECK(ctl(CMD_STOP, err) == 0 && M.n_stop == 0);
  reset(); enter_print(9);
  CHECK(ctl(CMD_STOP, err) == 0 && M.n_stop == 0);
  reset(); enter_print(8);
  CHECK(ctl(CMD_PAUSE, err) == 1 && M.n_pause == 0);      // already paused
  CHECK(ctl(CMD_RESUME, err) == 1 && M.n_resume == 1);
  reset(); enter_print(4);
  CHECK(ctl(CMD_RESUME, err) == 0 && !strcmp(err, "not paused") && M.n_resume == 0);
  reset(); enter_print(6);
  CHECK(ctl(CMD_PAUSE, err) == 1 && M.n_pause == 0);
  reset(); enter_print(7);
  CHECK(ctl(CMD_STOP, err) == 1 && M.n_stop == 0);

  // ---- stop -> window 3 once state reaches 0 ----
  reset(); enter_print(4);
  CHECK(ctl(CMD_STOP, err) == 1 && M.n_stop == 1);
  M.state = 7; tick(); tick();
  CHECK(M.n_setwin == 0);
  M.state = 0; tick();
  CHECK(M.n_setwin == 1 && M.win_idx[0] == 3);
  s = status(); CHECK(!strcmp(s.state, "idle"));   // not "starting" while leaving
  tick(); tick();
  CHECK(M.n_setwin == 1);                          // once
  mock_loop(); tick();
  s = status(); CHECK(!strcmp(s.state, "idle") && mock_printer() == NULL);

  // ---- a valid-generation start carrying a non-CTB magic never reaches window 16 ----
  reset();
  post_magic = 0x4550504eu; accept_start(path); tick(); post_magic = 0x12fd0106u;
  CHECK(M.n_setwin == 0 && !gkq_start_in_flight());
  s = status(); CHECK(!strcmp(s.message, "not a ctb file") && !strcmp(s.state, "idle"));
  reset();
  post_magic = 0; accept_start(path); tick(); post_magic = 0x12fd0106u;
  CHECK(M.n_setwin == 0 && !gkq_start_in_flight());
  reset(); post_magic = 0x12fd0107u; accept_start(path); tick(); post_magic = 0x12fd0106u;
  CHECK(M.n_setwin == 1);

  // ---- start fills the item and switches to window 16 ----
  reset(); mock_set_window(13, 13);
  accept_start(path);
  tick();
  CHECK(M.n_setwin == 1 && M.win_idx[0] == 16 && M.win_param[0] == (void*)mock_mem(GK_STATIC_ITEM));
  const unsigned char* it = mock_mem(GK_STATIC_ITEM);
  const char* base = strrchr(path, '/') + 1;
  CHECK(!strcmp((const char*)it, base));
  CHECK(!strcmp((const char*)it + 0x100, path));
  CHECK(it[0x500] == 0x06 && it[0x501] == 0x01 && it[0x502] == 0xfd && it[0x503] == 0x12 && it[0x504] == 0);
  int allz = 1;
  for (int i = (int)strlen(base) + 1; i < 0x100; i++) if (it[i]) allz = 0;
  for (int i = 0x100 + (int)strlen(path) + 1; i < 0x500; i++) if (it[i]) allz = 0;
  for (int i = 0x505; i < 0x50c; i++) if (it[i]) allz = 0;
  CHECK(allz);
  CHECK(gkq_start_in_flight());

  // starting stays reported until state reaches 3
  s = status(); CHECK(!strcmp(s.state, "starting"));
  mock_loop(); tick();                 // window 16 created, printer state 0
  s = status(); CHECK(!strcmp(s.state, "starting") && gkq_start_in_flight());
  M.state = 1; tick();
  s = status(); CHECK(!strcmp(s.state, "starting") && gkq_start_in_flight());   // HOME is not printing yet
  pub_at_end = -1; before = pub_count;
  M.state = 3; tick();
  CHECK(!gkq_start_in_flight());
  s = status(); CHECK(!strcmp(s.state, "printing") && !s.message[0]);
  CHECK(pub_at_end > before);          // snapshot (printing) published before end_start

  // ---- publish-before-end on failure as well ----
  reset(); enter_print(3);
  accept_start(path); pub_at_end = -1; before = pub_count;
  tick();
  CHECK(pub_at_end > before && !gkq_start_in_flight());

  // ---- start guards: busy msgbox, pending window switch, wrong window ----
  reset(); *mock_mem(GK_MSGBOX_BUSY) = 1; accept_start(path); tick();
  CHECK(M.n_setwin == 0 && !gkq_start_in_flight());
  reset(); mock_set_window(3, 13); accept_start(path); tick();
  CHECK(M.n_setwin == 0 && !gkq_start_in_flight());
  reset(); mock_set_window(1, 1); accept_start(path); tick();
  CHECK(M.n_setwin == 0 && !gkq_start_in_flight());

  // ---- window never reached / window closes before printing / error before printing ----
  reset(); accept_start(path); tick();
  CHECK(M.n_setwin == 1 && gkq_start_in_flight());
  fake_now += 4000; tick();
  CHECK(gkq_start_in_flight());
  fake_now += 2000; tick();
  CHECK(!gkq_start_in_flight()); s = status(); CHECK(!strcmp(s.message, "start failed") && !strcmp(s.state, "idle"));

  reset(); accept_start(path); tick(); mock_loop(); tick();
  CHECK(gkq_start_in_flight());
  fake_now += 60000; tick();                 // preheat has no timeout
  CHECK(gkq_start_in_flight());
  mock_set_window(16, 3); mock_loop(); tick();   // window closed (printer freed) before printing
  CHECK(!gkq_start_in_flight()); s = status(); CHECK(!strcmp(s.message, "print stopped on the printer"));

  reset(); accept_start(path); tick(); mock_loop(); M.state = 9; tick();
  CHECK(!gkq_start_in_flight()); s = status(); CHECK(!strcmp(s.message, "start failed") && !strcmp(s.state, "error"));

  // ---- stop while a start is in flight ----
  // 2a: file check still running (no CMD_START yet): cancel; the checker's later CMD_START is dropped
  reset();
  uint32_t ga = gkq_begin_start(path);
  tick();
  CHECK(ctl(CMD_STOP, err) == 1);
  CHECK(!gkq_start_in_flight() && M.n_stop == 0 && M.n_setwin == 0);
  s = status(); CHECK(!strcmp(s.message, "start cancelled") && !strcmp(s.state, "idle"));
  struct gk_cmd late;
  memset(&late, 0, sizeof late);
  late.kind = CMD_START; snprintf(late.path, sizeof late.path, "%s", path); late.gen = ga; late.magic = post_magic;
  gkq_post(&late); tick();
  CHECK(M.n_setwin == 0 && !gkq_start_in_flight());
  // the old checker finishing/failing late cannot touch a newer start
  uint32_t gb = gkq_begin_start(path);
  CHECK(gb != 0 && gb != ga && gkq_end_start_gen(ga, "late") == 0 && gkq_start_in_flight());
  // 2a with CMD_START queued but not yet taken: stop (drained after) still cancels cleanly
  reset();
  accept_start(path);  // posts CMD_START, tick not run yet
  CHECK(ctl(CMD_STOP, err) == 1);   // tick takes START first (window 16 requested), then STOP revokes
  CHECK(!gkq_start_in_flight() && M.n_stop == 0);
  s = status(); CHECK(!strcmp(s.message, "start cancelled"));
  CHECK(*(uint32_t*)mock_mem(GK_UI_NEXT) == *(uint32_t*)mock_mem(GK_UI_CURRENT));
  // 2b: window 16 requested, not reached: revoke with ui_set_window_index(current, 0)
  reset(); accept_start(path); tick();
  CHECK(M.n_setwin == 1 && M.win_idx[0] == 16 && gkq_start_in_flight());
  CHECK(ctl(CMD_STOP, err) == 1);
  CHECK(M.n_setwin == 2 && M.win_idx[1] == 3 && M.win_param[1] == NULL && M.n_stop == 0);
  CHECK(*(uint32_t*)mock_mem(GK_UI_NEXT) == 3);
  CHECK(!gkq_start_in_flight());
  s = status(); CHECK(!strcmp(s.message, "start cancelled") && !strcmp(s.state, "idle"));
  mock_loop(); tick();
  CHECK(mock_printer() == NULL && mock_ui_current() == 3);
  reset(); mock_set_window(13, 13); accept_start(path); tick();
  CHECK(ctl(CMD_STOP, err) == 1 && M.win_idx[1] == 13);   // revoke targets the current window
  // 2c: window 16, printer, HOME: printer_stop; when the window closes the start was cancelled
  reset(); accept_start(path); tick(); mock_loop(); tick();
  M.state = 1; tick();
  CHECK(gkq_start_in_flight());
  CHECK(ctl(CMD_STOP, err) == 1 && M.n_stop == 1);
  CHECK(gkq_start_in_flight());
  M.state = 0; tick();                              // stop from HOME goes straight to idle
  CHECK(M.n_setwin == 2 && M.win_idx[1] == 3);
  mock_loop(); tick();
  CHECK(!gkq_start_in_flight()); s = status();
  CHECK(!strcmp(s.message, "start cancelled") && !strcmp(s.state, "idle"));
  // 2c for the other stoppable states
  int stoppable[] = {3, 4, 5, 6, 8};
  for (unsigned i = 0; i < sizeof stoppable / sizeof stoppable[0]; i++) {
    reset(); accept_start(path); tick(); mock_loop(); M.state = stoppable[i]; tick();
    if (stoppable[i] >= 3 && stoppable[i] <= 5) { CHECK(!gkq_start_in_flight()); continue; }  // start already done
    CHECK(ctl(CMD_STOP, err) == 1 && M.n_stop == 1);
  }
  // 2d: heating dialog (window 16, printer, state 0): refuse, never stop, never leave the window
  reset(); accept_start(path); tick(); mock_loop(); tick();
  s = status(); CHECK(!strcmp(s.state, "starting"));
  CHECK(ctl(CMD_STOP, err) == 0 && !strcmp(err, "printer is heating - cancel it on the touchscreen"));
  CHECK(M.n_stop == 0 && M.n_setwin == 1 && gkq_start_in_flight());
  tick(); tick(); mock_loop();
  CHECK(M.n_setwin == 1 && mock_printer() != NULL && gkq_start_in_flight());
  s = status(); CHECK(!strcmp(s.state, "starting"));
  // 2d also for a touchscreen-started preheat (no start in flight)
  reset(); enter_print(0); tick();
  CHECK(ctl(CMD_STOP, err) == 0 && !strcmp(err, "printer is heating - cancel it on the touchscreen"));
  CHECK(M.n_stop == 0 && M.n_setwin == 0);
  // a stop with nothing in flight and nothing running is still "not printing"
  reset(); CHECK(ctl(CMD_STOP, err) == 0 && !strcmp(err, "not printing"));

  // ---- message accuracy: window closes after being reached, hook did not stop ----
  reset(); accept_start(path); tick(); mock_loop(); M.state = 1; tick();
  CHECK(gkq_start_in_flight());
  mock_set_window(16, 3); mock_loop(); tick();       // stopped on the touchscreen
  CHECK(!gkq_start_in_flight());
  s = status(); CHECK(!strcmp(s.message, "print stopped on the printer") && !strcmp(s.state, "idle"));
  // never reached window 16: still "start failed"
  reset(); accept_start(path); tick(); fake_now += 6000; tick();
  s = status(); CHECK(!strcmp(s.message, "start failed"));

  // ---- stale CMD_START dropped ----
  reset();
  post_start(path);                       // nothing in flight
  tick();
  CHECK(M.n_setwin == 0 && !gkq_start_in_flight());
  CHECK(gkq_begin_start("a") != 0);
  struct gk_cmd old;
  memset(&old, 0, sizeof old);
  old.kind = CMD_START; snprintf(old.path, sizeof old.path, "%s", path); old.gen = gkq_start_gen();
  gkq_end_start("file check timed out");
  CHECK(gkq_begin_start("b") != 0);       // a newer start is in flight
  gkq_post(&old);                         // the older checker finally posts
  tick();
  CHECK(M.n_setwin == 0 && gkq_start_in_flight());   // dropped, newer start untouched

  // ---- in-flight lifetime keyed to the checker (F1) ----
  // a slow full check (> the old flat 30 s) is still in flight, then still starts
  reset();
  uint32_t g0 = gkq_begin_start("x");
  CHECK(g0 != 0);
  tick(); fake_now += 100000; tick();
  CHECK(gkq_start_in_flight());
  post_start(path); tick();
  CHECK(M.n_setwin == 1 && gkq_start_in_flight());
  // checker finished without posting: short safety net, counted from when it finished
  reset();
  g0 = gkq_begin_start("x");
  tick(); fake_now += 100000; tick();
  gkq_checker_done(g0); tick();
  fake_now += 9000; tick();
  CHECK(gkq_start_in_flight());
  fake_now += 2000; tick();
  CHECK(!gkq_start_in_flight()); s = status(); CHECK(!strcmp(s.message, "start failed") && !strcmp(s.state, "idle"));
  // hard cap from begin_start for a hung check
  reset();
  CHECK(gkq_begin_start("x") != 0);
  tick(); fake_now += 599000; tick();
  CHECK(gkq_start_in_flight());
  fake_now += 2000; tick();
  CHECK(!gkq_start_in_flight()); s = status(); CHECK(!strcmp(s.message, "file check timed out") && !strcmp(s.state, "idle"));
  CHECK(gkq_begin_start("y") != 0); tick(); fake_now += 590000; tick();   // new start: its own full cap
  CHECK(gkq_start_in_flight());
  // a stale checker finishing late must not end a newer start
  reset();
  g0 = gkq_begin_start("a");
  tick();
  gkq_end_start_gen(g0, "file check timed out");
  uint32_t g1 = gkq_begin_start("b");
  CHECK(g1 != 0 && g1 != g0);
  CHECK(gkq_end_start_gen(g0, "late failure") == 0);
  gkq_checker_done(g0);
  tick(); fake_now += 100000; tick();
  CHECK(gkq_start_in_flight());
  s = status(); CHECK(!s.message[0] && !strcmp(s.state, "starting"));

  // ---- a finished/errored print window is flagged for the server (F2) ----
  reset(); enter_print(2); tick(); s = status(); CHECK(s.print_window_open == 1);
  reset(); enter_print(9); tick(); s = status(); CHECK(s.print_window_open == 1);
  reset(); tick(); s = status(); CHECK(s.print_window_open == 0);

  // ---- a stale start-failure message clears once the tick sees printing (F7) ----
  reset();
  CHECK(gkq_end_start_gen(gkq_begin_start("x"), "start failed") == 1);
  enter_print(0); fake_now += 300; tick(); s = status();
  CHECK(!strcmp(s.message, "start failed"));        // not printing yet: kept
  M.state = 1; fake_now += 300; tick(); s = status(); CHECK(!strcmp(s.message, "start failed"));
  M.state = 3; fake_now += 300; tick(); s = status();                // a touchscreen-started print
  CHECK(!strcmp(s.state, "printing") && !s.message[0]);
  reset(); enter_print(3); tick();                  // refusal while printing keeps its message
  accept_start(path); tick(); s = status(); CHECK(!strcmp(s.message, "printer busy"));

  // ---- tick always chains the original ----
  reset();
  int n0 = M.n_app_update;
  tick(); post_start(path); tick(); enter_print(4); tick();
  CHECK(M.n_app_update == n0 + 3);

  // ---- publish rate: quiet ticks do not republish before 250 ms ----
  reset(); tick(); int pc = pub_count; tick(); tick();
  CHECK(pub_count == pc);
  fake_now += 260; tick();
  CHECK(pub_count == pc + 1);

  // ---- structural: the socket side never touches the printer ----
  reset(); enter_print(4); tick();
  int calls = mock_calls_printer_or_ui();
  struct gk_status rd;
  gkq_read_status(&rd); gkq_read_status(&rd);
  CHECK(mock_calls_printer_or_ui() == calls);

  test_tick_routes_z_and_reports();
  test_tick_start_refused_while_z_moving();
  test_tick_start_then_z_same_drain();
  test_tick_start_dismisses_finished_print();

  unlink(path);
  if (fails) { fprintf(stderr, "%d failure(s)\n", fails); return 1; }
  puts("test_tick OK");
  return 0;
}
