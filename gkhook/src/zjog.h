// Remote Z jog decisions. UI tick only: the only caller of axis_* in the hook.
#ifndef GKHOOK_ZJOG_H
#define GKHOOK_ZJOG_H
#include <stddef.h>

#include "queue.h"

// What the tick knows about the printer, filled by tick.c.
struct gk_zview {
  int cur, next;          // ui.current / ui.next window indices
  int printer;            // *(void**)0x101754 != NULL
  int start_in_flight;    // a print start is in flight
};

// 1 = accepted and issued; 0 = refused (err holds a short message). c->kind is CMD_Z_*.
int gk_zjog_cmd(const struct gk_cmd* c, const struct gk_zview* v, char* err, size_t errn)
    __attribute__((visibility("hidden")));
// Fills z_avail / z_homed / z_moving / z_pos.
void gk_zjog_status(struct gk_status* s) __attribute__((visibility("hidden")));
// Set by the installer once the axis table checks out; 0 = every z_* is refused.
extern int gk_z_enabled __attribute__((visibility("hidden")));
#endif
