// UI-thread tick: drains commands, acts on the printer, publishes the status snapshot.
// Runs inside chitu's ui_loop via ui.update_cb (findings 6.1). Must be cheap, never block,
// never abort, and always chain the original callback.
#include "tick.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#include "chitu_abi.h"
#include "log.h"
#include "queue.h"
#include "zjog.h"

#define WIN_MAIN     3
#define WIN_EXPLORER 13
#define WIN_PRINT    16

// printer_t state enum (findings 4)
enum { S_IDLE = 0, S_HOME = 1, S_COMPLETE = 2, S_STAGE1 = 3, S_STAGE2 = 4, S_STAGE3 = 5,
       S_PAUSING = 6, S_STOPPING = 7, S_SUSPEND = 8, S_ERROR = 9 };

#define PUBLISH_MS     250
#define WINDOW_WAIT_MS 5000    // ui_set_window_index(16) must take effect within this
#define POST_WAIT_MS   10000   // checker finished but no CMD_START arrived (safety net)
#define CHECK_CAP_MS   600000  // hard cap from begin_start (a hung print_file_check)

// explorer_item_tag layout (findings 2)
#define ITEM_SIZE  0x50c
#define ITEM_PATH  0x100
#define ITEM_MAGIC 0x500
#define ITEM_ISDIR 0x504

__attribute__((visibility("hidden"))) int64_t (*gk_tick_clock_ms)(void);

static int stopping;        // we sent printer_stop; send the user back to the main window
static int stop_sent;       // we sent printer_stop during this start (message accuracy)
static int req;             // 0 none, 1 window 16 requested, 2 in window 16 waiting for printing
static int64_t req_t0;
static int seen_flight;     // a start is in flight and has no request of its own yet
static uint32_t seen_gen;
static int64_t flight_t0;
static int seen_done;       // when we first saw the checker of seen_gen finished
static int64_t done_t0;
static int was_printing;    // state 3/4/5 at the previous snapshot
static int dirty = 1;
static int64_t last_pub;
static int published_once;
static int last_state_logged = -2;

static int64_t now_ms(void) {
  if (gk_tick_clock_ms) return gk_tick_clock_ms();
  struct timespec t;
  clock_gettime(CLOCK_MONOTONIC, &t);
  return (int64_t)t.tv_sec * 1000 + t.tv_nsec / 1000000;
}

void gk_tick_reset(void) {
  stopping = stop_sent = req = seen_flight = seen_done = was_printing = published_once = 0;
  seen_gen = 0;
  dirty = 1;
  last_pub = req_t0 = flight_t0 = done_t0 = 0;
  last_state_logged = -2;
}

struct view {
  uint32_t cur, next;
  void* p;       // printer_t* or NULL
  int state;     // -1 when there is no printer in window 16
};

static void read_view(struct view* v) {
  v->cur = gk_rd32(GK_UI_CURRENT);
  v->next = gk_rd32(GK_UI_NEXT);
  v->p = gk_rdptr(GK_PRINTER_PTR);
  v->state = (v->p && v->cur == WIN_PRINT) ? gk_printer_get_state(v->p) : -1;
}

static int is_printing(int s) { return s == S_STAGE1 || s == S_STAGE2 || s == S_STAGE3; }

static const char* state_name(const struct view* v) {
  switch (v->state) {
    case S_HOME: case S_STAGE1: case S_STAGE2: case S_STAGE3: case S_PAUSING: case S_STOPPING:
      return "printing";
    case S_SUSPEND: return "paused";
    case S_COMPLETE: return "complete";
    case S_ERROR: return "error";
    case S_IDLE:
      // Window 16 with a printer still at 0: preheat / not yet started (also a start made on
      // the touchscreen). Not while we are stopping or the window is already being left.
      return (stopping || v->next != v->cur) ? "idle" : "starting";
    default: return "idle";
  }
}

static const char* phase_name(int state) {
  switch (state) {
    case S_HOME: return "homing";
    case S_STAGE1: case S_STAGE2: case S_STAGE3: return "exposing";
    case S_PAUSING: return "pausing";
    case S_STOPPING: return "stopping";
    default: return "";
  }
}

static void build_status(struct gk_status* s, const struct view* v) {
  memset(s, 0, sizeof *s);
  s->layer = s->total_layers = s->elapsed_s = s->remaining_s = -1;
  s->progress = -1;
  snprintf(s->state, sizeof s->state, "%s", state_name(v));
  // panel size: chitu's own globals, read here because only the tick may touch chitu memory
  uint16_t rx = gk_rd16(GK_RES_X), ry = gk_rd16(GK_RES_Y);
  if (rx > 0 && rx <= 20000 && ry > 0 && ry <= 20000) { s->screen_x = rx; s->screen_y = ry; }
  gk_zjog_status(s);
  if (v->state < 0) return;
  snprintf(s->phase, sizeof s->phase, "%s", phase_name(v->state));
  s->print_window_open = 1;  // window 16 with a printer: a finished print may still be on screen
  // path of the running job: a plain C string valid while the print window lives
  const char* path = (const char*)GK_ADDR(GK_PRINT_CONTEXT_PATH);
  size_t n = strnlen(path, 1023);
  if (n) {
    memcpy(s->file, path, n);
    s->have_file = 1;
  }
  int layer = gk_printer_get_current_layer(v->p);
  if (layer >= 0) s->layer = layer;
  // printer_get_file_info does no NULL check (returns handle+0x1004), so test the handle.
  if (*(void* const*)((const char*)v->p + GK_PRINTER_FILE_OFF)) {
    const unsigned char* info = gk_printer_get_file_info(v->p);
    if (info) {
      uint32_t total = *(const uint32_t*)(info + 12);
      if (total > 0 && total < 10000000u) {
        s->total_layers = (long)total;
        if (s->layer >= 0) {
          double pr = 100.0 * (double)s->layer / (double)total;
          s->progress = pr < 0 ? 0 : pr > 100 ? 100 : pr;
        }
      }
    }
  }
  uint64_t el = gk_printer_get_print_tick(v->p);
  uint64_t tot = gk_printer_get_total_print_tick(v->p);
  s->elapsed_s = (long)(el / 1000);
  if (tot > 0) s->remaining_s = tot > el ? (long)((tot - el) / 1000) : 0;  // unknown while no estimate
}

static void publish(const struct view* v, int64_t now) {
  struct gk_status s;
  build_status(&s, v);
  gkq_publish_status(&s);
  last_pub = now;
  dirty = 0;
  published_once = 1;
  // A print we did not start (touchscreen) must not show an old start failure beside it.
  int pr = is_printing(v->state);
  if (pr && !was_printing) gkq_clear_message();
  was_printing = pr;
  if (v->state != last_state_logged) {  // transitions only
    last_state_logged = v->state;
    gk_log("gkhook: state %d window %u (%s)", v->state, (unsigned)v->cur, s.state);
  }
}

// Publish the new snapshot first, then clear the in-flight flag, so no poll sees a stale idle.
static void end_start(const struct view* v, int64_t now, const char* m) {
  publish(v, now);
  if (m) gk_log("gkhook: start ended: %s", m);
  gkq_end_start(m);
  req = 0;
}

// Stop while a start is in flight but chitu has no print window yet: end the start for that
// generation (publish first, as everywhere). The checker's later CMD_START is then dropped
// by gkq_start_valid() because the generation is no longer in flight.
static void cancel_start(const struct view* v, int64_t now, uint32_t gen) {
  publish(v, now);
  gk_log("gkhook: start cancelled");
  gkq_end_start_gen(gen, "start cancelled");
  req = 0;
  seen_flight = 0;
}

static const char* basename_of(const char* p) {
  const char* s = strrchr(p, '/');
  return s ? s + 1 : p;
}

static void cmd_start(const struct gk_cmd* c, struct view* v, int64_t now) {
  if (!gkq_start_valid(c->gen)) {  // stale: expired, or from an older start
    gk_log("gkhook: dropped stale start");
    return;
  }
  if (req || (v->cur != WIN_MAIN && v->cur != WIN_EXPLORER) || v->p || v->next != v->cur ||
      gk_rd8(GK_MSGBOX_BUSY) != 0) {
    end_start(v, now, "printer busy");
    return;
  }
  if (gk_z_enabled && !gk_axis_is_idle(0)) {  // backstop for the server's own check
    end_start(v, now, "Z is moving");
    return;
  }
  size_t pl = strlen(c->path);
  if (pl >= 1024) { end_start(v, now, "bad file path"); return; }  // item.path holds 1023 + NUL
  // Magic comes from the checker thread (no file I/O here). Re-check it: a non-CTB item
  // reaches chitu's NULL-printer crash path (findings 2).
  uint32_t magic = c->magic;
  if (magic != 0x12fd0106u && magic != 0x12fd0107u) { end_start(v, now, "not a ctb file"); return; }
  unsigned char* item = GK_ADDR(GK_STATIC_ITEM);
  memset(item, 0, ITEM_SIZE);
  const char* base = basename_of(c->path);
  size_t bl = strnlen(base, 255);
  memcpy(item, base, bl);                 // item was zeroed: NUL-terminated, name <= 255
  memcpy(item + ITEM_PATH, c->path, pl);
  memcpy(item + ITEM_MAGIC, &magic, 4);  // ARM is little-endian: same as the u32 chitu reads
  item[ITEM_ISDIR] = 0;
  gk_ui_set_window_index(WIN_PRINT, item);
  v->next = WIN_PRINT;  // later commands in this drain see the pending switch
  req = 1;
  stop_sent = 0;
  req_t0 = now;
  dirty = 1;
  gk_log("gkhook: start requested");
}

// pause / resume / stop; exactly one result per command.
static void cmd_ctl(const struct gk_cmd* c, struct view* v, int64_t now) {
  int s = v->state;
  const char* err = NULL;
  uint32_t fg;
  int fdone;
  if (s < 0 && c->kind == CMD_STOP && gkq_flight(&fg, &fdone)) {
    // A start is in flight and chitu has no print window with a printer yet.
    if (req == 1 && v->next != v->cur) {
      // Window 16 requested but not reached. ui_loop only switches when next != current, so
      // setting next back to current cancels the pending switch.
      gk_ui_set_window_index(v->cur, NULL);
      v->next = v->cur;
    }
    // (req == 0: the file check is still running or CMD_START not yet taken; nothing to undo.)
    cancel_start(v, now, fg);
  } else if (s < 0) {
    err = "not printing";
  } else if (c->kind == CMD_PAUSE) {
    if (is_printing(s)) { gk_printer_pause(v->p); v->state = S_PAUSING; }
    else if (s == S_HOME) err = "printer is homing - pause when printing starts";
    else if (s != S_PAUSING && s != S_SUSPEND) err = "not printing";
  } else if (c->kind == CMD_RESUME) {
    if (s == S_SUSPEND) gk_printer_resume(v->p);
    else err = "not paused";
  } else {  // CMD_STOP
    if (s == S_IDLE && !stopping && v->next == v->cur) {
      // Window 16, printer at state 0: the stock heat dialog (preheat) or the instant before
      // printer_start. Stock's heat-dialog cancel (app_msgbox_heat_tip 0x1ca40-0x1ca70) does
      // ui_set_window_index(3,0) AND returns 1 to close its own msgbox. The hook cannot close
      // that msgbox, and leaving window 16 under an open heat dialog risks the dialog later
      // calling printer_start on a destroyed printer (0x1cb20/0x1cb28 read print_context+0x514).
      // So neither printer_stop nor a window change: the owner cancels on the touchscreen.
      err = "printer is heating - cancel it on the touchscreen";
    } else if (s == S_HOME || is_printing(s) || s == S_PAUSING || s == S_SUSPEND) {
      gk_printer_stop(v->p);
      v->state = S_STOPPING;
      stopping = 1;
      stop_sent = 1;
    } else if (s != S_STOPPING) {
      err = "not printing";  // idle/complete/error/no printer: never send stop (risk 7)
    }
  }
  gkq_set_result(c->id, err == NULL, err);
  dirty = 1;
}

// z_move / z_top / z_home / z_stop; exactly one result per command. The decision (guards,
// clamp) lives in zjog.c; the printer/start state comes from this tick's own view.
static void cmd_z(const struct gk_cmd* c, const struct view* v) {
  struct gk_zview zv;
  char err[64] = "";
  zv.cur = (int)v->cur;
  zv.next = (int)v->next;
  zv.printer = v->p != NULL;
  zv.start_in_flight = req != 0 || gkq_start_in_flight();
  int ok = gk_zjog_cmd(c, &zv, err, sizeof err);
  gkq_set_result(c->id, ok, ok ? NULL : err);
  dirty = 1;  // publish right away so the page sees the new z state
}

static void run(void) {
  struct view v;
  struct gk_cmd c;
  int64_t now = now_ms();
  read_view(&v);

  while (gkq_take(&c)) {
    if (c.kind == CMD_START) cmd_start(&c, &v, now);
    else if (c.kind == CMD_Z_MOVE || c.kind == CMD_Z_TOP || c.kind == CMD_Z_HOME ||
             c.kind == CMD_Z_STOP) cmd_z(&c, &v);
    else if (c.kind != CMD_STATUS) cmd_ctl(&c, &v, now);
  }
  if (v.state < 0) stopping = 0;

  // stop: once the printer is done, leave the print window as the stock dialog does
  if (stopping && (v.state == S_IDLE || v.state == S_COMPLETE || v.state == S_ERROR)) {
    if (v.next == v.cur) { gk_ui_set_window_index(WIN_MAIN, NULL); v.next = WIN_MAIN; }
    stopping = 0;
    dirty = 1;
  }

  if (req) {
    if (v.state >= 0) req = 2;
    if (req == 2 && is_printing(v.state)) { publish(&v, now); gkq_end_start(NULL); req = 0; }
    else if (req == 2 && v.state < 0)  // window 16 was reached, then closed
      end_start(&v, now, stop_sent ? "start cancelled" : "print stopped on the printer");
    else if (req == 2 && (v.state == S_COMPLETE || v.state == S_ERROR)) end_start(&v, now, "start failed");
    else if (req == 1 && now - req_t0 > WINDOW_WAIT_MS) end_start(&v, now, "start failed");
  } else {
    uint32_t g;
    int done;
    if (!gkq_flight(&g, &done)) {
      seen_flight = seen_done = 0;
    } else {
      if (!seen_flight || g != seen_gen) { seen_flight = 1; seen_gen = g; flight_t0 = now; seen_done = 0; }
      if (done && !seen_done) { seen_done = 1; done_t0 = now; }
      const char* m = NULL;
      if (now - flight_t0 > CHECK_CAP_MS) m = "file check timed out";
      else if (seen_done && now - done_t0 > POST_WAIT_MS) m = "start failed";
      // generation-keyed: never ends a newer start that began since we looked
      if (m) {
        publish(&v, now);  // snapshot first, as in end_start
        if (gkq_end_start_gen(g, m)) gk_log("gkhook: start failed: %s", m);
        seen_flight = 0;
      }
    }
  }

  if (dirty || !published_once || now - last_pub >= PUBLISH_MS) publish(&v, now);
}

void gk_tick(void) {
  run();
  gk_app_update();  // every path chains the stock callback (app_msgbox_run)
}
