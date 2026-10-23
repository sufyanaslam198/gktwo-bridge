// Fixed addresses and typed wrappers for chitu V1.3.3 (non-PIE, va 0x10000).
// Functions: checked against /work/re/nm.txt. Globals missing from nm: docs/reverse-engineering notes.
// Only valid after the fingerprint passes (gk_abi_enabled).
#ifndef GKHOOK_CHITU_ABI_H
#define GKHOOK_CHITU_ABI_H
#include <assert.h>
#include <stdint.h>

extern int gk_abi_enabled __attribute__((visibility("hidden")));

// The single test seam: production uses the fixed chitu addresses; the host tests build
// with -DGK_ABI_INDIRECT and supply gk_abi_resolve(), which maps an address to a fake
// function or fake memory (tests/mock_chitu.c).
#ifdef GK_ABI_INDIRECT
void* gk_abi_resolve(uintptr_t addr);
#define GK_ADDR(a) (gk_abi_resolve((uintptr_t)(a)))
#else
#define GK_ADDR(a) ((void*)(uintptr_t)(a))
#endif

// printer_* functions (nm; findings 6.1). UI-tick only.
#define GK_PRINTER_START             0x0003a1fcUL
#define GK_PRINTER_PAUSE             0x0003a224UL
#define GK_PRINTER_STOP              0x0003a248UL
#define GK_PRINTER_RESUME            0x0003a26cUL
#define GK_PRINTER_GET_STATE         0x0003ac18UL
#define GK_PRINTER_GET_CURRENT_LAYER 0x0003ac20UL
#define GK_PRINTER_GET_FILE_INFO     0x0003ac48UL
#define GK_PRINTER_GET_TOTAL_TICK    0x0003ac68UL  // entry points (nm), not findings 4 evidence addrs
#define GK_PRINTER_GET_PRINT_TICK    0x0003ac98UL

// UI window switch (nm; findings 6.1). UI-tick only.
#define GK_UI_SET_WINDOW_INDEX       0x0004ffe0UL

// print_file_* (nm; findings 6.1). Allowed off the UI thread (R6).
#define GK_PRINT_FILE_GET_FORMAT     0x00030554UL
#define GK_PRINT_FILE_OPEN           0x000305d8UL
#define GK_PRINT_FILE_CLOSE          0x0003073cUL
#define GK_PRINT_FILE_CHECK          0x00030a5cUL

// ui struct and update callback slot (findings 4, 6.1; ui in nm).
#define GK_UI                        0x001078e8UL
#define GK_UI_CURRENT                0x001078ecUL  // ui+4 (findings 6.1)
#define GK_UI_NEXT                   0x001078f0UL  // ui+8
#define GK_UI_UPDATE_CB              0x001078f8UL
#define GK_APP_UPDATE                0x00013d3cUL  // original callback (nm _Z10app_updatev)

// Globals (findings 4, 6.1; msgbox_is_busy/machine_param/static item in nm).
#define GK_PRINTER_PTR               0x00101754UL  // printer_t*, NULL when no print window
#define GK_PRINT_CONTEXT_PATH        0x00101348UL  // print_context + 0x100
#define GK_MSGBOX_BUSY               0x00101224UL
#define GK_MACHINE_PARAM             0x001094e8UL
#define GK_STATIC_ITEM               0x00100c78UL  // stock print-file item buffer (0x50c)

// printer_t field (findings 4): print_file* handle. printer_get_file_info returns
// handle+0x1004 without a NULL check, so the tick tests the handle first.
#define GK_PRINTER_FILE_OFF          0x000000bcUL

// Printer FPGA resolution globals (nm, u16 each). print_file_* may read them off-thread (R6).
#define GK_RES_X                     0x00104a24UL
#define GK_RES_Y                     0x00104a26UL

// Axis layer (findings chitu-manual-move.md section 8; functions in nm). Axis 0 = Z. UI-tick only.
#define GK_AXIS_MOVE                 0x00024bf8UL  // (axis, flags 1=relative 0=absolute, float mm, float mm/s)
#define GK_AXIS_HOME                 0x00024d28UL
#define GK_AXIS_HALT                 0x00024b9cUL
#define GK_AXIS_IS_IDLE              0x000255d8UL
#define GK_AXIS_IS_HOME              0x0002562cUL
#define GK_AXIS_GET_POSITION         0x00025664UL  // returns float mm (s0)
#define GK_Z_TRAVEL                  0x001094f8UL  // float, machine_param+0x10
#define GK_Z_SPEED                   0x001095acUL  // float, machine_param+0xc4
#define GK_AXIS_TABLE                0x00101804UL  // axis pointer table; [0] == GK_AXIS0_OBJ
#define GK_AXIS0_OBJ                 0x00108e90UL

// No assert in the ARM .so: nothing in the hook may abort inside chitu (build uses -DNDEBUG).
#define GK_CHECK() assert(gk_abi_enabled)

static inline uint32_t gk_rd32(uintptr_t a) { return *(volatile uint32_t*)GK_ADDR(a); }
static inline uint16_t gk_rd16(uintptr_t a) { return *(volatile uint16_t*)GK_ADDR(a); }
static inline uint8_t  gk_rd8(uintptr_t a)  { return *(volatile uint8_t*)GK_ADDR(a); }
static inline void*    gk_rdptr(uintptr_t a) { return *(void* volatile*)GK_ADDR(a); }
static inline float    gk_rdf32(uintptr_t a) { return *(volatile float*)GK_ADDR(a); }

static inline void  gk_printer_pause(void* p)  { GK_CHECK(); ((void (*)(void*))GK_ADDR(GK_PRINTER_PAUSE))(p); }
static inline void  gk_printer_stop(void* p)   { GK_CHECK(); ((void (*)(void*))GK_ADDR(GK_PRINTER_STOP))(p); }
static inline void  gk_printer_resume(void* p) { GK_CHECK(); ((void (*)(void*))GK_ADDR(GK_PRINTER_RESUME))(p); }
static inline int   gk_printer_get_state(void* p) { GK_CHECK(); return ((int (*)(void*))GK_ADDR(GK_PRINTER_GET_STATE))(p); }
static inline int   gk_printer_get_current_layer(void* p) { GK_CHECK(); return ((int (*)(void*))GK_ADDR(GK_PRINTER_GET_CURRENT_LAYER))(p); }
static inline void* gk_printer_get_file_info(void* p) { GK_CHECK(); return ((void* (*)(void*))GK_ADDR(GK_PRINTER_GET_FILE_INFO))(p); }
// Both tick getters return u64 milliseconds in r0:r1 (disassembly 0x3ac68/0x3ac98: ldrd / r0,r1).
static inline uint64_t gk_printer_get_total_print_tick(void* p) { GK_CHECK(); return ((uint64_t (*)(void*))GK_ADDR(GK_PRINTER_GET_TOTAL_TICK))(p); }
static inline uint64_t gk_printer_get_print_tick(void* p) { GK_CHECK(); return ((uint64_t (*)(void*))GK_ADDR(GK_PRINTER_GET_PRINT_TICK))(p); }
static inline void  gk_ui_set_window_index(int idx, void* param) { GK_CHECK(); ((void (*)(int, void*))GK_ADDR(GK_UI_SET_WINDOW_INDEX))(idx, param); }
static inline void  gk_app_update(void) { ((void (*)(void))GK_ADDR(GK_APP_UPDATE))(); }

// print_file_*. get_format: nonzero = known format (0x30554 returns the table index or 0).
// open: handle or NULL (0x305d8). check: 0 = OK (see validate.c), close: frees the handle (0x3073c).
static inline int   gk_print_file_get_format(const char* path) { GK_CHECK(); return ((int (*)(const char*))GK_ADDR(GK_PRINT_FILE_GET_FORMAT))(path); }
static inline void* gk_print_file_open(const char* path) { GK_CHECK(); return ((void* (*)(const char*))GK_ADDR(GK_PRINT_FILE_OPEN))(path); }
static inline int   gk_print_file_check(void* f) { GK_CHECK(); return ((int (*)(void*))GK_ADDR(GK_PRINT_FILE_CHECK))(f); }
static inline void  gk_print_file_close(void* f) { GK_CHECK(); ((void (*)(void*))GK_ADDR(GK_PRINT_FILE_CLOSE))(f); }

// Axis calls: ARM hard-float ABI, so float args/return are declared float (s0/s1).
static inline void  gk_axis_move(int axis, int flags, float mm, float speed) { GK_CHECK(); ((void (*)(int, int, float, float))GK_ADDR(GK_AXIS_MOVE))(axis, flags, mm, speed); }
static inline void  gk_axis_home(int axis) { GK_CHECK(); ((void (*)(int))GK_ADDR(GK_AXIS_HOME))(axis); }
static inline void  gk_axis_halt(int axis) { GK_CHECK(); ((void (*)(int))GK_ADDR(GK_AXIS_HALT))(axis); }
static inline int   gk_axis_is_idle(int axis) { GK_CHECK(); return ((int (*)(int))GK_ADDR(GK_AXIS_IS_IDLE))(axis); }
static inline int   gk_axis_is_home(int axis) { GK_CHECK(); return ((int (*)(int))GK_ADDR(GK_AXIS_IS_HOME))(axis); }
static inline float gk_axis_get_position(int axis) { GK_CHECK(); return ((float (*)(int))GK_ADDR(GK_AXIS_GET_POSITION))(axis); }

#endif
