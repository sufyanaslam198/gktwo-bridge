// Print control: SDCP Cmd 128 start, 129 pause, 130 stop, 131 resume, forwarded to gkhook through the hook
// worker; 132, 133, 386 and 387 do not apply to this printer.
#include "files.h"
#include "history.h"
#include "hookw.h"
#include "loop.h"
#include "sdcp.h"
#include "status.h"
#include "upload.h"
#include "usbcopy.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#define START_GUARD_MS 1000    // snapshots lag up to 250 ms: right after an accepted start, state is still idle

struct pctx {                  // per queued hook command; carries the connection id, never a pointer to it
  uint32_t conn;
  int cmd;
  char rid[257];
  char client[sizeof g_bridge.client_file];   // Cmd 128 only
  int usb;                                    // the start that follows a USB copy: the Cmd 128 reply is long sent
};

static int g_start_inflight;                  // a start is queued, its reply is not back yet
static long long g_accepted_ms;               // when the hook last accepted a start
static int g_watch;                           // an accepted start has not shown its outcome yet
static unsigned long g_watch_seq;             // poll number when the hook accepted it (hookw_call_seq)
static char g_usb_dest[600];                  // the copy being made for a USB print, and the client path it came from
static char g_usb_client[sizeof g_bridge.client_file];

static int g_usb_created;                     // the copy did not exist before: remove it if the print never starts
static int g_usb_copied;                      // the copy is complete and its print has not been accepted yet
static int g_usb_inflight;                    // the hook start that follows the copy is queued
static int g_usb_cancel;                      // Cmd 130 arrived while that start was in flight

static int error_number_for(const char *msg);
static void usb_fail(const char *err);
static int running_state(const char *s) {
  return !strcmp(s, "starting") || !strcmp(s, "printing") || !strcmp(s, "paused");
}

static long long now_ms(void) {
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return (long long)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

// gkhook error -> Ack for Cmd 128.
static int start_ack(const char *err) {
  static const struct { const char *e; int ack; } t[] = {
    { "file not found", 2 }, { "file is not in the files folder", 2 }, { "bad file path", 2 },
    { "cannot read file", 4 }, { "cannot open file", 4 }, { "cannot start file check", 4 },
    { "file resolution does not match printer", 5 },
    { "not a ctb file", 6 }, { "unsupported file format", 6 }, { "unsupported ctb version", 6 },
    { "only .ctb files can be printed", 6 },
  };
  for (size_t i = 0; i < sizeof t / sizeof t[0]; i++)
    if (strcmp(err, t[i].e) == 0) return t[i].ack;
  return 1;                                   // printer busy, hook starting, Z is moving, anything else
}

static void refuse(const struct sdcp_req *r, int ack, const char *why) {
  sdcp_reply(r->conn, r->cmd, r->rid, ack, "");
  sdcp_notice(why);
}

// Client path -> the hook's start line, the canonical client path and the file name for history. Refuses
// (false, with the reason for the notice) anything but a file in the top level of /local/.
static int start_target(const char *client, char *line, size_t cap, char *canon, size_t ccap, const char **base,
                        const char **why) {
  char abs[1100];
  int st;
  *why = "bad file path";
  if (files_resolve(client, abs, sizeof abs, &st) < 0 || files_client_path(abs, canon, ccap) < 0) return -1;
  if (st != ST_LOCAL) { *why = "only files in /local/ can be printed"; return -1; }
  const char *name = canon + 7;                 // after "/local/"
  if (!*name) return -1;                        // the storage root
  if (strchr(name, '/')) { *why = "move the file to the top folder to print it"; return -1; }
  struct jw w;
  jw_init(&w, line, cap);
  jw_raw(&w, "{\"cmd\":\"start\",\"path\":");
  jw_str(&w, abs);
  jw_raw(&w, "}");
  *base = name;
  return w.err ? -1 : 0;
}

static void done(void *ctx, int ok, const char *err) {
  struct pctx *p = ctx;
  if (p->cmd == 128) {
    g_start_inflight = 0;
    if (p->usb) { g_usb_inflight = 0; g_usb_copied = 0; }
    if (ok) {
      if (p->usb) g_bridge.usb_copying = 0;
      g_watch_seq = hookw_call_seq();
      g_accepted_ms = now_ms();
      g_watch = 1;
      g_bridge.error_number = 0;
      snprintf(g_bridge.client_file, sizeof g_bridge.client_file, "%s", p->client);
      sdcp_status_dirty();
    } else if (p->usb) {
      g_usb_copied = 1;                         // the copy is still there to remove
      usb_fail(err);
    } else {
      history_abort();
    }
  }
  if (p->usb) {
    free(p);
    if (ok && g_usb_cancel) {                   // Cmd 130 came after the copy: the print it asked to stop has just started
      struct pctx *q = calloc(1, sizeof *q);
      if (q) {
        q->cmd = 130;                           // no reply is owed; usb keeps done() from sending one
        q->usb = 1;
        if (hookw_call("{\"cmd\":\"stop\"}", done, q) < 0) free(q);
      }
    }
    g_usb_cancel = 0;
    return;
  }
  if (ok) {
    sdcp_reply(p->conn, p->cmd, p->rid, 0, "");
  } else {
    sdcp_reply(p->conn, p->cmd, p->rid, p->cmd == 128 ? start_ack(err) : 1, "");
    sdcp_notice(err);
  }
  free(p);
}

// Queue line for r; if it cannot be queued, answer at once.
static int queue(const struct sdcp_req *r, const char *line, const char *client) {
  struct pctx *p = calloc(1, sizeof *p);
  if (p) {
    p->conn = r->conn;
    p->cmd = r->cmd;
    snprintf(p->rid, sizeof p->rid, "%s", r->rid);
    if (client) snprintf(p->client, sizeof p->client, "%s", client);
    if (hookw_call(line, done, p) == 0) return 0;
    free(p);
  }
  refuse(r, 1, "printer busy");
  return -1;
}

// The print for a USB file cannot go ahead (copy failed or cancelled, or the hook refused the start). The task
// is closed as stopped and the copy removed if this print created it. If a print started meanwhile on the
// touchscreen, the open task already follows that print (history_track took over its file): it is handed to
// it, not closed, and nothing is announced.
static void usb_fail(const char *err) {
  struct hk_status h;
  hookw_status(&h);
  g_bridge.usb_copying = 0;
  g_usb_inflight = 0;
  g_usb_cancel = 0;
  if (g_usb_copied && g_usb_created && strcmp(h.file, g_usb_dest) != 0) unlink(g_usb_dest);
  g_usb_copied = 0;
  if (running_state(h.state)) {
    history_adopt(&h);
    g_bridge.client_file[0] = 0;
  } else {
    history_end(3, 0, 0);
    g_bridge.sub_status = 8;
    g_bridge.error_number = error_number_for(err);
    if (strcmp(err, "cancelled") != 0) sdcp_notice(err);
  }
  sdcp_status_dirty();
}

// The copy ended: start the print with the copy, or close the task.
static void usb_done(int ok, const char *err) {
  if (ok) {
    g_usb_copied = 1;
    struct hk_status h;
    hookw_status(&h);
    if (!running_state(h.state)) {
      char line[700];
      struct jw w;
      jw_init(&w, line, sizeof line);
      jw_raw(&w, "{\"cmd\":\"start\",\"path\":");
      jw_str(&w, g_usb_dest);
      jw_raw(&w, "}");
      struct pctx *p = w.err ? NULL : calloc(1, sizeof *p);
      if (p) {
        p->cmd = 128;
        p->usb = 1;
        snprintf(p->client, sizeof p->client, "%s", g_usb_client);
        if (hookw_call(line, done, p) == 0) {   // usb_copying stays until it answers
          g_start_inflight = 1;
          g_usb_inflight = 1;
          return;
        }
        free(p);
      }
    }
    err = "printer busy";
  }
  usb_fail(err);
}

static int is_ctb_name(const char *name) {
  size_t n = strlen(name);
  return n >= 4 && strcasecmp(name + n - 4, ".ctb") == 0;
}

// Cmd 128 for /usb/...: checks, then the copy to internal storage; usb_done starts the print.
static void usb_start(const struct sdcp_req *r, const char *abs) {
  char client[sizeof g_bridge.client_file], dest[600];
  const char *base = strrchr(abs, '/') + 1;
  struct stat sb;
  long long size;
  if (stat(abs, &sb) < 0) { refuse(r, 2, "file not found"); return; }
  if (!S_ISREG(sb.st_mode) || !is_ctb_name(base)) { refuse(r, 6, "only .ctb files can be printed"); return; }
  int rc = usbcopy_probe(abs, &size);
  if (rc == -1) { refuse(r, 4, "cannot read file"); return; }
  if (rc == -2) { refuse(r, 6, "not a ctb file"); return; }

  struct hk_status h;
  hookw_status(&h);
  int idle = !strcmp(h.state, "idle") || !strcmp(h.state, "complete") || !strcmp(h.state, "error");
  if (snprintf(dest, sizeof dest, "%s/%s", files_root(ST_LOCAL), base) >= (int)sizeof dest ||
      files_client_path(abs, client, sizeof client) < 0) { refuse(r, 2, "bad file path"); return; }
  if (!h.hook_ok) { refuse(r, 1, "printer state unknown"); return; }          // the touchscreen can print anything
  if (!idle || !strcmp(h.file, dest)) { refuse(r, 1, "printer busy"); return; }   // never the file gkhook has open
  if (!upload_storage_ok()) { refuse(r, 1, "storage not mounted"); return; }
  if (!files_space_ok(files_root(ST_LOCAL), size)) { refuse(r, 4, "not enough space on internal storage"); return; }

  if (!history_begin(base, client)) { refuse(r, 1, "printer busy"); return; }
  snprintf(g_usb_dest, sizeof g_usb_dest, "%s", dest);
  snprintf(g_usb_client, sizeof g_usb_client, "%s", client);
  struct stat ex;
  g_usb_created = lstat(dest, &ex) < 0;
  g_usb_copied = 0;
  g_usb_cancel = 0;
  const char *why = "printer busy";
  int bc = usbcopy_begin(abs, base, usb_done, &why);
  if (bc < 0) {
    history_abort();
    refuse(r, bc == -2 ? 1 : 4, why);
    return;
  }
  g_bridge.usb_copying = 1;
  sdcp_reply(r->conn, r->cmd, r->rid, 0, "");
  sdcp_status_dirty();
}

static void cmd_start(struct sdcp_req *r) {
  char given[1100], abs[1100], client[sizeof g_bridge.client_file], line[2048];
  long long layer = 0;
  int t = r->data >= 0 ? jget(r->d, r->data, "Filename") : -1;
  if (t < 0 || jstr(r->d, t, given, sizeof given) < 0) { refuse(r, 2, "bad file path"); return; }
  int st = ST_LOCAL, rc = files_resolve(given, abs, sizeof abs, &st);
  if (rc == -2) { refuse(r, 2, "USB drive not mounted"); return; }
  int usb = rc == 0 && st == ST_USB;
  const char *base, *why;
  if (!usb && start_target(given, line, sizeof line, client, sizeof client, &base, &why) < 0) { refuse(r, 2, why); return; }
  t = jget(r->d, r->data, "StartLayer");
  if (t >= 0 && (jint(r->d, t, &layer) < 0 || layer != 0)) { refuse(r, 1, "start layer is not supported"); return; }
  if (upload_busy() || g_bridge.usb_copying || cmd_print_start_pending()) {
    refuse(r, 1, "printer busy");
    return;
  }
  if (usb) { usb_start(r, abs); return; }
  if (!history_begin(base, client)) { refuse(r, 1, "printer busy"); return; }   // a live print owns the task
  if (queue(r, line, client) < 0) { history_abort(); return; }
  g_start_inflight = 1;
}

int cmd_print_start_pending(void) {
  return g_start_inflight || usbcopy_active(NULL, NULL) || now_ms() - g_accepted_ms < START_GUARD_MS;
}

static void cmd_simple(struct sdcp_req *r) {
  if (r->cmd == 130 && usbcopy_active(NULL, NULL)) {       // a USB copy is the print: stopping it needs no hook call
    usbcopy_cancel();
    sdcp_reply(r->conn, r->cmd, r->rid, 0, "");
    return;
  }
  if (r->cmd == 130 && g_usb_inflight) {                   // copy done, start not answered yet: stop it once it is
    g_usb_cancel = 1;
    sdcp_reply(r->conn, r->cmd, r->rid, 0, "");
    return;
  }
  const char *c = r->cmd == 129 ? "pause" : r->cmd == 130 ? "stop" : "resume";
  char line[32];
  snprintf(line, sizeof line, "{\"cmd\":\"%s\"}", c);
  queue(r, line, NULL);
}

static void cmd_not_applicable(struct sdcp_req *r) { sdcp_reply(r->conn, r->cmd, r->rid, 1, ""); }
static void cmd_camera(struct sdcp_req *r) { sdcp_reply(r->conn, r->cmd, r->rid, 2, ",\"VideoUrl\":\"\""); }

static int error_number_for(const char *msg) {
  if (!strcmp(msg, "file check failed") || !strcmp(msg, "cannot open file") || !strcmp(msg, "cannot read file")) return 2;
  if (!strcmp(msg, "not a ctb file")) return 4;
  return 0;
}

// Snapshot listener: an accepted start that ends without a print leaves a message, announced once. Only
// snapshots polled after the hook accepted the start count (gkhook clears the message when it accepts one).
// The file check can fail before any poll shows starting: the task is closed and Status 8 set here too.
static void on_snapshot(void) {
  if (!g_watch) return;
  struct hk_status h;
  if (hookw_status(&h) <= g_watch_seq) return;      // polled before the start was accepted
  if (!strcmp(h.state, "printing") || !strcmp(h.state, "paused")) { g_watch = 0; return; }
  if (!h.message[0] || running_state(h.state)) return;
  g_watch = 0;
  g_bridge.error_number = error_number_for(h.message);
  g_bridge.sub_status = 8;
  history_end_begun();
  sdcp_notice(h.message);
  sdcp_status_dirty();
}

void cmd_print_init(void) {
  sdcp_register(128, cmd_start);
  sdcp_register(129, cmd_simple);
  sdcp_register(130, cmd_simple);
  sdcp_register(131, cmd_simple);
  sdcp_register(132, cmd_not_applicable);
  sdcp_register(133, cmd_not_applicable);
  sdcp_register(386, cmd_camera);
  sdcp_register(387, cmd_not_applicable);
  if (hookw_on_change(on_snapshot) < 0) fprintf(stderr, "gksdcp: no free hook change slot for print control\n");
}
