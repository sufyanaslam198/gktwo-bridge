// Fake chitu for host tests: fake memory at the real addresses and fake functions behind
// gk_abi_resolve() (built with -DGK_ABI_INDIRECT).
#ifndef MOCK_CHITU_H
#define MOCK_CHITU_H
#include <stdint.h>

struct mock {
  // printer
  int state, layer;
  uint64_t print_tick, total_tick;
  uint32_t total_layers;
  int has_file;
  // calls
  int n_pause, n_resume, n_stop, n_state, n_layer, n_info, n_ticks, n_app_update;
  int n_setwin;
  int win_idx[16];
  void* win_param[16];
  // print_file_*
  int fmt, open_ok, check_rc, n_open, n_close, n_check, n_getfmt;
  uint32_t fx, fy, fver;
  // axis 0 (Z): state the fake axis_* functions report, and every axis_move/home/halt call
  int ax_homed, ax_idle;
  float ax_pos;
  int n_axis;                       // recorded calls (move/home/halt only)
  struct { int kind, axis, flags; float mm, speed; } ax[16];
  // msgbox: app_msgbox_close_avtive_msgbox calls, and window 16 left with a dialog open
  int n_msgbox_close, left_with_dialog;
};
enum { AX_MOVE = 1, AX_HOME = 2, AX_HALT = 3 };
extern struct mock M;

void mock_reset(void);
uint8_t* mock_mem(uintptr_t addr);
uint32_t mock_ui_current(void);
void mock_set_window(uint32_t cur, uint32_t next);  // raw ui.current / ui.next
void mock_loop(void);                               // what ui_loop does for a pending switch
void mock_axis_params(float travel, float speed);   // machine_param floats
void* mock_printer(void);                           // current printer_t* (or NULL)
int mock_calls_printer_or_ui(void);                 // any printer_* / ui call so far
// Queue one dialog with callback cb; shown = already on screen (msgbox_is_busy), else
// app_msgbox_run (the chained app_update) shows it on the next tick.
void mock_dialog(uint32_t cb, int shown);
#endif
