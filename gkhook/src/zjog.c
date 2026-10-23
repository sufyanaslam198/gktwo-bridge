// Remote Z jog: decides whether a z command may move the build plate and issues it.
// Runs on the UI tick only. Every move is clamped to [0, travel]; machine_param floats are
// validated before use because they come straight from chitu's memory.
#include "zjog.h"

#include <stdio.h>

#include "chitu_abi.h"

#define WIN_PRINT    16
#define WIN_FIRMWARE 6
#define MAX_TRAVEL   300.0f  // mm; larger (or non-finite) means the setting is garbage
#define MAX_SPEED    30.0f   // mm/s
#define MIN_STEP     0.01f   // mm; a smaller remaining delta counts as "at the limit"

__attribute__((visibility("hidden"))) int gk_z_enabled = 0;

// NaN fails every comparison, so these also reject NaN; infinity fails the upper bound.
static int travel_sane(float t) { return t > 0.0f && t <= MAX_TRAVEL; }
static int speed_sane(float s) { return s > 0.0f && s <= MAX_SPEED; }
static int finite_f(float x) { return x == x && x - x == 0.0f; }

static int refuse(char* err, size_t errn, const char* msg) {
  if (err && errn) snprintf(err, errn, "%s", msg);
  return 0;
}

int gk_zjog_cmd(const struct gk_cmd* c, const struct gk_zview* v, char* err, size_t errn) {
  if (!gk_z_enabled) return refuse(err, errn, "Z control unavailable");
  if (v->printer || v->cur == WIN_PRINT || v->next == WIN_PRINT || v->start_in_flight)
    return refuse(err, errn, "printer busy");
  if (c->kind != CMD_Z_MOVE && c->kind != CMD_Z_TOP && c->kind != CMD_Z_HOME && c->kind != CMD_Z_STOP)
    return refuse(err, errn, "bad command");
  // a firmware update drives Z by G-code: even a halt must not interfere
  if (v->cur == WIN_FIRMWARE || v->next == WIN_FIRMWARE)
    return refuse(err, errn, "firmware update in progress");
  if (c->kind == CMD_Z_STOP) {  // halting an idle axis is harmless; nothing else to check
    gk_axis_halt(0);
    return 1;
  }
  float speed = gk_rdf32(GK_Z_SPEED);
  if (!speed_sane(speed)) return refuse(err, errn, "Z speed setting invalid");
  if (!gk_axis_is_idle(0)) return refuse(err, errn, "Z is moving");
  if (c->kind == CMD_Z_HOME) {  // stock order; unhomed is exactly when this is needed
    gk_axis_halt(0);
    gk_axis_home(0);
    return 1;
  }
  if (!gk_axis_is_home(0)) return refuse(err, errn, "home Z first");

  float travel = gk_rdf32(GK_Z_TRAVEL);
  int up = c->kind == CMD_Z_TOP || c->mm > 0;
  if (up && !travel_sane(travel)) return refuse(err, errn, "Z travel limit unknown");
  float pos = gk_axis_get_position(0);
  if (!finite_f(pos)) return refuse(err, errn, "Z position unknown");

  if (c->kind == CMD_Z_TOP) {
    if (travel - pos < MIN_STEP) return refuse(err, errn, "at top limit");
    gk_axis_move(0, 0, travel, speed);  // absolute, sane travel only
    return 1;
  }
  float target = pos + (float)c->mm;
  if (up) { if (target > travel) target = travel; }
  else if (target < 0.0f) target = 0.0f;
  float delta = target - pos;
  if (up ? delta < MIN_STEP : -delta < MIN_STEP)
    return refuse(err, errn, up ? "at top limit" : "at bottom limit");
  // |delta| <= |mm| <= 100 by construction; belt and braces against a non-finite result
  if (!finite_f(delta) || delta > 100.0f || delta < -100.0f)
    return refuse(err, errn, "Z position unknown");
  gk_axis_move(0, 1, delta, speed);
  return 1;
}

void gk_zjog_status(struct gk_status* s) {
  s->z_avail = gk_z_enabled ? 1 : 0;
  s->z_homed = s->z_moving = 0;
  s->z_pos = 0;
  if (!gk_z_enabled) return;
  s->z_moving = gk_axis_is_idle(0) ? 0 : 1;
  s->z_homed = gk_axis_is_home(0) ? 1 : 0;
  if (s->z_homed) s->z_pos = (double)gk_axis_get_position(0);
}
