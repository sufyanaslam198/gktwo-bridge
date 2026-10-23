#include "mock_chitu.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../src/chitu_abi.h"

struct mock M;
int gk_abi_enabled = 1;

#define BASE 0x100000u
static uint64_t memw[0x10000 / 8];
static unsigned char* mem(void) { return (unsigned char*)memw; }
static void* fake_printer;
static unsigned char fake_file[0x2000] __attribute__((aligned(8)));

uint8_t* mock_mem(uintptr_t a) { return mem() + (a - BASE); }
void* mock_printer(void) { return fake_printer; }
uint32_t mock_ui_current(void) { return *(uint32_t*)mock_mem(GK_UI_CURRENT); }
void mock_set_window(uint32_t cur, uint32_t next) {
  *(uint32_t*)mock_mem(GK_UI_CURRENT) = cur;
  *(uint32_t*)mock_mem(GK_UI_NEXT) = next;
}

void gk_log(const char* fmt, ...) {
  if (!getenv("MOCK_LOG")) return;
  va_list ap;
  va_start(ap, fmt);
  vfprintf(stderr, fmt, ap);
  va_end(ap);
  fputc('\n', stderr);
}

static void free_printer(void) {
  free(fake_printer);
  fake_printer = NULL;
  *(void**)mock_mem(GK_PRINTER_PTR) = NULL;
}

void mock_reset(void) {
  free_printer();
  memset(&M, 0, sizeof M);
  memset(mem(), 0, sizeof memw);
  M.fmt = 1; M.open_ok = 1; M.fx = 2560; M.fy = 1600; M.fver = 6; M.total_layers = 100;
  M.has_file = 1;
  *(uint16_t*)mock_mem(GK_RES_X) = 2560;
  *(uint16_t*)mock_mem(GK_RES_Y) = 1600;
  mock_set_window(3, 3);
  M.ax_homed = 1; M.ax_idle = 1; M.ax_pos = 50.0f;
  mock_axis_params(150.0f, 5.0f);
  *(uint32_t*)mock_mem(GK_AXIS_TABLE) = GK_AXIS0_OBJ;
}

void mock_axis_params(float travel, float speed) {
  *(float*)mock_mem(GK_Z_TRAVEL) = travel;
  *(float*)mock_mem(GK_Z_SPEED) = speed;
}

void mock_loop(void) {
  uint32_t cur = mock_ui_current(), next = *(uint32_t*)mock_mem(GK_UI_NEXT);
  if (cur == next) return;
  if (cur == 16) free_printer();
  *(uint32_t*)mock_mem(GK_UI_CURRENT) = next;
  if (next == 16) {
    fake_printer = calloc(1, 0x300);
    if (M.has_file) *(void**)((char*)fake_printer + GK_PRINTER_FILE_OFF) = fake_file;
    *(void**)mock_mem(GK_PRINTER_PTR) = fake_printer;
    M.state = 0;
    strcpy((char*)mock_mem(GK_PRINT_CONTEXT_PATH), (char*)mock_mem(GK_STATIC_ITEM) + 0x100);
  }
}

int mock_calls_printer_or_ui(void) {
  return M.n_pause + M.n_resume + M.n_stop + M.n_state + M.n_layer + M.n_info + M.n_ticks + M.n_setwin;
}

// ---- fake functions ----
static void f_pause(void* p) { (void)p; M.n_pause++; }
static void f_resume(void* p) { (void)p; M.n_resume++; }
static void f_stop(void* p) { (void)p; M.n_stop++; }
static int f_state(void* p) { (void)p; M.n_state++; return M.state; }
static int f_layer(void* p) { (void)p; M.n_layer++; return M.layer; }
static void* f_info(void* p) { (void)p; M.n_info++; *(uint32_t*)(fake_file + 0x1010) = M.total_layers; return fake_file + 0x1004; }
static uint64_t f_total(void* p) { (void)p; M.n_ticks++; return M.total_tick; }
static uint64_t f_tick(void* p) { (void)p; M.n_ticks++; return M.print_tick; }
static void f_setwin(int idx, void* param) {
  if (M.n_setwin < 16) { M.win_idx[M.n_setwin] = idx; M.win_param[M.n_setwin] = param; }
  M.n_setwin++;
  *(uint32_t*)mock_mem(GK_UI_NEXT) = (uint32_t)idx;
}
static void ax_rec(int kind, int axis, int flags, float mm, float speed) {
  if (M.n_axis < 16) {
    M.ax[M.n_axis].kind = kind; M.ax[M.n_axis].axis = axis; M.ax[M.n_axis].flags = flags;
    M.ax[M.n_axis].mm = mm; M.ax[M.n_axis].speed = speed;
  }
  M.n_axis++;
}
static void f_axmove(int a, int fl, float mm, float sp) { ax_rec(AX_MOVE, a, fl, mm, sp); }
static void f_axhome(int a) { ax_rec(AX_HOME, a, 0, 0, 0); }
static void f_axhalt(int a) { ax_rec(AX_HALT, a, 0, 0, 0); }
static int f_axidle(int a) { (void)a; return M.ax_idle; }
static int f_axishome(int a) { (void)a; return M.ax_homed; }
static float f_axpos(int a) { (void)a; return M.ax_pos; }
static void f_app(void) { M.n_app_update++; }
static int f_getfmt(const char* p) { (void)p; M.n_getfmt++; return M.fmt; }
static void* f_open(const char* p) {
  (void)p;
  M.n_open++;
  if (!M.open_ok) return NULL;
  *(uint32_t*)(fake_file + 0x1004) = M.fx;
  *(uint32_t*)(fake_file + 0x1008) = M.fy;
  *(uint32_t*)(fake_file + 0x1010) = M.total_layers;
  *(uint32_t*)(fake_file + 0x1e10) = M.fver;
  return fake_file;
}
static int f_check(void* f) { (void)f; M.n_check++; return M.check_rc; }
static void f_close(void* f) { (void)f; M.n_close++; }

void* gk_abi_resolve(uintptr_t a) {
  if (a >= BASE && a < BASE + sizeof memw) return mock_mem(a);
  switch (a) {
    case GK_PRINTER_PAUSE: return (void*)f_pause;
    case GK_PRINTER_RESUME: return (void*)f_resume;
    case GK_PRINTER_STOP: return (void*)f_stop;
    case GK_PRINTER_GET_STATE: return (void*)f_state;
    case GK_PRINTER_GET_CURRENT_LAYER: return (void*)f_layer;
    case GK_PRINTER_GET_FILE_INFO: return (void*)f_info;
    case GK_PRINTER_GET_TOTAL_TICK: return (void*)f_total;
    case GK_PRINTER_GET_PRINT_TICK: return (void*)f_tick;
    case GK_UI_SET_WINDOW_INDEX: return (void*)f_setwin;
    case GK_AXIS_MOVE: return (void*)f_axmove;
    case GK_AXIS_HOME: return (void*)f_axhome;
    case GK_AXIS_HALT: return (void*)f_axhalt;
    case GK_AXIS_IS_IDLE: return (void*)f_axidle;
    case GK_AXIS_IS_HOME: return (void*)f_axishome;
    case GK_AXIS_GET_POSITION: return (void*)f_axpos;
    case GK_APP_UPDATE: return (void*)f_app;
    case GK_PRINT_FILE_GET_FORMAT: return (void*)f_getfmt;
    case GK_PRINT_FILE_OPEN: return (void*)f_open;
    case GK_PRINT_FILE_CHECK: return (void*)f_check;
    case GK_PRINT_FILE_CLOSE: return (void*)f_close;
  }
  fprintf(stderr, "mock: unmapped address 0x%lx\n", (unsigned long)a);
  abort();
}
