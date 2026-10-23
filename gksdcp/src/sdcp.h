// SDCP message envelope: request parsing, dispatch table, response and push frames.
#ifndef GKSDCP_SDCP_H
#define GKSDCP_SDCP_H
#include "json.h"
#include <stddef.h>
#include <stdint.h>

struct conn;

struct sdcp_req {
  uint32_t conn;
  int cmd;
  char rid[257];             // RequestID, up to 256 bytes
  const struct jdoc *d;
  int data;                  // token of Data.Data, -1 if absent
};
typedef void (*sdcp_fn)(struct sdcp_req *r);
void sdcp_register(int cmd, sdcp_fn fn);

// Response frame. extra is "" or a JSON fragment starting with "," placed after "Ack":n in Data.Data.
// Safe if the connection has closed.
void sdcp_reply(uint32_t conn, int cmd, const char *rid, int ack, const char *extra);

// Status and attributes frames now: to one connection, or to all with conn 0.
void sdcp_push_status(uint32_t conn);
void sdcp_push_attributes(uint32_t conn);
// Something changed: push to all, at most one per 500 ms per kind, the latest state always goes out.
void sdcp_status_dirty(void);
void sdcp_attributes_dirty(void);
void sdcp_notice(const char *msg);     // Type 100, to all
void sdcp_error(int code);             // sdcp/error, to all

// Installs on_ws_text, registers Cmd 0 and 1, hooks hookw changes and starts the push timer.
// Call before loop_run.
void sdcp_init(void);
void sdcp_on_text(struct conn *c, const char *s, size_t n);

// Print control, Cmd 128-133, 386, 387 (cmd_print.c). Called by sdcp_init before it subscribes its own
// snapshot listener, so a start failure is recorded before the status push.
void cmd_print_init(void);
// 1 while a start is queued, a USB copy runs, or a start was accepted within the last second (the snapshot may still say idle).
int cmd_print_start_pending(void);

// Z movement, project extension commands (cmd_z.c): Z_MOVE takes {"Mm": 1, 10, 100, -1, -10 or -100}.
enum { SDCP_CMD_Z_HOME = 4096, SDCP_CMD_Z_MOVE = 4097, SDCP_CMD_Z_TOP = 4098, SDCP_CMD_Z_STOP = 4099 };
void cmd_z_init(void);

// File list, delete and rename, Cmd 258, 259, 192 (cmd_files.c). Needs files_init first.
void cmd_files_init(void);
#endif
