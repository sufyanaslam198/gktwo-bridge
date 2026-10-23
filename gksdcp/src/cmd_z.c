// Z movement extension, project range 4096-4099, forwarded to gkhook's z_home / z_move / z_top / z_stop through
// the hook worker. gkhook enforces every Z safety rule; this file only checks the input and keeps out of the way
// of an upload, a USB copy and a start in progress.
#include "hookw.h"
#include "sdcp.h"
#include "status.h"
#include "upload.h"
#include "usbcopy.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

struct zctx {                  // per queued hook command; carries the connection id, never a pointer to it
  uint32_t conn;
  int cmd;
  char rid[257];
};

static void refuse(const struct sdcp_req *r, const char *why) {
  sdcp_reply(r->conn, r->cmd, r->rid, 1, "");
  sdcp_notice(why);
}

static void done(void *ctx, int ok, const char *err) {
  struct zctx *z = ctx;
  if (ok) {
    sdcp_reply(z->conn, z->cmd, z->rid, 0, "");
    sdcp_status_dirty();
  } else {
    sdcp_reply(z->conn, z->cmd, z->rid, 1, "");
    sdcp_notice(err);
  }
  free(z);
}

static int step_ok(long long mm) {
  return mm == 1 || mm == 10 || mm == 100 || mm == -1 || mm == -10 || mm == -100;
}

static void cmd_z(struct sdcp_req *r) {
  char line[48];
  if (r->cmd == SDCP_CMD_Z_MOVE) {
    long long mm = 0;
    int t = r->data >= 0 ? jget(r->d, r->data, "Mm") : -1;
    if (t < 0 || jint(r->d, t, &mm) < 0 || !step_ok(mm)) { refuse(r, "bad request"); return; }
    snprintf(line, sizeof line, "{\"cmd\":\"z_move\",\"mm\":%lld}", mm);
  } else {
    snprintf(line, sizeof line, "{\"cmd\":\"%s\"}",
             r->cmd == SDCP_CMD_Z_HOME ? "z_home" : r->cmd == SDCP_CMD_Z_TOP ? "z_top" : "z_stop");
  }
  // A stop is always forwarded: gkhook decides, the bridge never blocks it.
  if (r->cmd != SDCP_CMD_Z_STOP &&
      (upload_busy() || g_bridge.usb_copying || cmd_print_start_pending())) {
    refuse(r, "printer busy");
    return;
  }
  struct zctx *z = calloc(1, sizeof *z);
  if (z) {
    z->conn = r->conn;
    z->cmd = r->cmd;
    snprintf(z->rid, sizeof z->rid, "%s", r->rid);
    if (hookw_call(line, done, z) == 0) return;
    free(z);
  }
  refuse(r, "printer busy");
}

void cmd_z_init(void) {
  sdcp_register(SDCP_CMD_Z_HOME, cmd_z);
  sdcp_register(SDCP_CMD_Z_MOVE, cmd_z);
  sdcp_register(SDCP_CMD_Z_TOP, cmd_z);
  sdcp_register(SDCP_CMD_Z_STOP, cmd_z);
}
