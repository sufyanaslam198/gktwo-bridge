// The UI-thread tick. Installed into ui.update_cb; always chains the stock callback.
// Only tick.c touches printer_*, the ui struct, 0x101754 and ui_set_window_index.
#ifndef GKHOOK_TICK_H
#define GKHOOK_TICK_H
#include <stdint.h>
void gk_tick(void) __attribute__((visibility("hidden")));

// Tests only.
extern int64_t (*gk_tick_clock_ms)(void) __attribute__((visibility("hidden")));  // NULL = CLOCK_MONOTONIC
void gk_tick_reset(void) __attribute__((visibility("hidden")));
#endif
