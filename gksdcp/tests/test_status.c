#include "clock.h"
#include "hookw.h"
#include "ident.h"
#include "json.h"
#include "status.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define CHECK(c) do { if (!(c)) { fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #c); exit(1); } } while (0)

static jsmntok_t toks[256];
static struct jdoc d;
static char out[16384];

static void idle_hk(struct hk_status *h) {
  hookw_parse_reply("{\"ok\":true,\"state\":\"idle\",\"file\":null,\"layer\":null,\"total_layers\":null,"
                    "\"progress\":null,\"elapsed_s\":null,\"remaining_s\":null,\"z\":null,\"phase\":null,\"screen\":null}", h);
}

static void with_state(struct hk_status *h, const char *state, const char *phase) {
  idle_hk(h);
  snprintf(h->state, sizeof h->state, "%s", state);
  snprintf(h->phase, sizeof h->phase, "%s", phase);
}

static void expect(const struct hk_status *h, const struct bridge_state *b, int cur, int sub) {
  if (status_current(h, b) != cur || status_sub(h, b) != sub) {
    fprintf(stderr, "state %s/%s: got %d/%d want %d/%d\n", h->state, h->phase,
            status_current(h, b), status_sub(h, b), cur, sub);
    exit(1);
  }
}

static int frame(const struct hk_status *h, const struct bridge_state *b, int attrs) {
  struct jw w;
  jw_init(&w, out, sizeof out);
  if (attrs) attributes_write(&w, h); else status_write(&w, h, b);
  CHECK(!w.err);
  int rc = jdoc_parse(&d, out, w.len, toks, 256);
  if (rc) fprintf(stderr, "frame: %s\n", out);
  return rc;
}

static long long num(const char *path) {
  long long v = -12345;
  int t = jpath(&d, path);
  CHECK(t >= 0 && jint(&d, t, &v) == 0);
  return v;
}

static int is_null(int t) { return t >= 0 && d.t[t].type == JSMN_PRIMITIVE && d.js[d.t[t].start] == 'n'; }

static int count_nulls(void) {
  int n = 0;
  for (const char *p = out; (p = strstr(p, "null")); p++) n++;
  return n;
}

int main(void) {
  struct hk_status h;
  struct bridge_state b;
  char s[700];

  // ---- clock ----
  CHECK(clock_now() > 0);                       // never 0 before anything is learned
  clock_learn(5);                               // out of range: ignored
  clock_learn(4102444801LL);
  CHECK(clock_now() > 1600000000LL);            // still the system clock
  clock_learn(1800000000LL);
  CHECK(clock_now() >= 1800000000LL && clock_now() <= 1800000003LL);
  clock_learn(1900000000000LL);                 // milliseconds
  CHECK(clock_now() >= 1900000000LL && clock_now() <= 1900000003LL);
  clock_learn(1704067199LL);                    // just below the range
  CHECK(clock_now() >= 1900000000LL);

  // ---- hook reply parsing ----
  hookw_parse_reply("{\"ok\":true,\"state\":\"printing\",\"file\":\"/media/emmc/x.ctb\",\"layer\":12,\"total_layers\":400,"
                    "\"progress\":3.00,\"elapsed_s\":95,\"remaining_s\":3010,"
                    "\"z\":{\"pos\":12.50,\"homed\":true,\"moving\":false},\"phase\":\"exposing\",\"screen\":[7680,4320],\"message\":\"m\"}", &h);
  CHECK(h.hook_ok == 1 && strcmp(h.state, "printing") == 0 && strcmp(h.file, "/media/emmc/x.ctb") == 0);
  CHECK(h.layer == 12 && h.total_layers == 400 && h.elapsed_s == 95 && h.remaining_s == 3010);
  CHECK(h.z_avail == 1 && h.z_homed == 1 && h.z_moving == 0 && h.z_pos > 12.49 && h.z_pos < 12.51);
  CHECK(strcmp(h.phase, "exposing") == 0 && h.screen_x == 7680 && h.screen_y == 4320 && strcmp(h.message, "m") == 0);
  idle_hk(&h);
  CHECK(h.hook_ok == 1 && h.layer == -1 && h.total_layers == -1 && h.elapsed_s == -1 && h.remaining_s == -1);
  CHECK(h.z_avail == 0 && h.screen_x == -1 && h.screen_y == -1 && h.file[0] == 0 && h.phase[0] == 0);
  hookw_parse_reply("{\"ok\":false,\"error\":\"hook starting\"}", &h);
  CHECK(h.hook_ok == 0 && strcmp(h.hook_error, "hook starting") == 0 && strcmp(h.state, "idle") == 0);
  hookw_parse_reply("not json", &h);
  CHECK(h.hook_ok == 0 && h.hook_error[0] && strcmp(h.state, "idle") == 0);
  hookw_parse_reply("{\"ok\":true}", &h);                  // no state
  CHECK(h.hook_ok == 0);

  // ---- status table, one row each ----
  memset(&b, 0, sizeof b);
  b.sub_status = 0;
  with_state(&h, "idle", "");
  expect(&h, &b, 0, 0);
  b.sub_status = 8;                             // idle keeps the last sub-status (after a stop)
  expect(&h, &b, 0, 8);
  b.uploading = 1;
  expect(&h, &b, 2, 8);
  with_state(&h, "starting", "");         expect(&h, &b, 1, 10);
  with_state(&h, "printing", "homing");   expect(&h, &b, 1, 1);
  with_state(&h, "printing", "pausing");  expect(&h, &b, 1, 5);
  with_state(&h, "printing", "stopping"); expect(&h, &b, 1, 7);
  with_state(&h, "printing", "exposing"); expect(&h, &b, 1, 3);
  with_state(&h, "printing", "");         expect(&h, &b, 1, 3);
  with_state(&h, "paused", "");           expect(&h, &b, 1, 6);
  with_state(&h, "complete", "");         expect(&h, &b, 0, 9);
  with_state(&h, "error", "");            expect(&h, &b, 0, 8);
  b.uploading = 1;                              // a print state wins over an upload
  with_state(&h, "printing", "exposing"); expect(&h, &b, 1, 3);
  b.usb_copying = 1;                            // usb copy wins over everything
  with_state(&h, "printing", "exposing"); expect(&h, &b, 1, 10);
  with_state(&h, "idle", "");             expect(&h, &b, 1, 10);
  memset(&b, 0, sizeof b);

  // ---- status frame ----
  ident_set_sysfs_root("/nonexistent");
  CHECK(ident_init("nonexistent0", "0000aabbccddeeff", "/tmp") == 0);
  hookw_parse_reply("{\"ok\":true,\"state\":\"printing\",\"file\":\"/media/emmc/x.ctb\",\"layer\":12,\"total_layers\":400,"
                    "\"progress\":3.00,\"elapsed_s\":95,\"remaining_s\":3010,"
                    "\"z\":{\"pos\":null,\"homed\":false,\"moving\":false},\"phase\":\"exposing\",\"screen\":[7680,4320]}", &h);
  CHECK(frame(&h, &b, 0) == 0);
  CHECK(jarr_n(&d, jpath(&d, "Status.CurrentStatus")) == 1);
  CHECK(num("Status.PrintInfo.Status") == 3);
  CHECK(num("Status.PrintInfo.CurrentLayer") == 12 && num("Status.PrintInfo.TotalLayer") == 400);
  CHECK(num("Status.PrintInfo.CurrentTicks") == 95000);
  CHECK(num("Status.PrintInfo.TotalTicks") == (95 + 3010) * 1000);
  CHECK(jstr(&d, jpath(&d, "Status.PrintInfo.Filename"), s, sizeof s) == 0 && strcmp(s, "/local/x.ctb") == 0);
  CHECK(jstr(&d, jpath(&d, "Topic"), s, sizeof s) == 0 && strcmp(s, "sdcp/status/0000aabbccddeeff") == 0);
  CHECK(jstr(&d, jpath(&d, "MainboardID"), s, sizeof s) == 0 && strcmp(s, "0000aabbccddeeff") == 0);
  CHECK(num("TimeStamp") > 0);
  CHECK(is_null(jpath(&d, "Status.GKBridge.Z.Pos")));        // not homed
  CHECK(num("Status.PreviousStatus") == 0 && num("Status.PrintScreen") == 0);
  CHECK(count_nulls() == 1);                                 // only GKBridge.Z.Pos

  // started over SDCP: the client's file name and task id
  snprintf(b.client_file, sizeof b.client_file, "/local/mine.ctb");
  snprintf(b.task_id, sizeof b.task_id, "0123456789abcdef0123456789abcdef");
  CHECK(frame(&h, &b, 0) == 0);
  CHECK(jstr(&d, jpath(&d, "Status.PrintInfo.Filename"), s, sizeof s) == 0 && strcmp(s, "/local/mine.ctb") == 0);
  CHECK(jstr(&d, jpath(&d, "Status.PrintInfo.TaskId"), s, sizeof s) == 0 && strcmp(s, "0123456789abcdef0123456789abcdef") == 0);

  // idle: no TaskId, zeros, GKBridge.Z null when unavailable
  memset(&b, 0, sizeof b);
  snprintf(b.task_id, sizeof b.task_id, "0123456789abcdef0123456789abcdef");
  idle_hk(&h);
  CHECK(frame(&h, &b, 0) == 0);
  CHECK(jstr(&d, jpath(&d, "Status.PrintInfo.TaskId"), s, sizeof s) == 0 && s[0] == 0);
  CHECK(num("Status.PrintInfo.CurrentLayer") == 0 && num("Status.PrintInfo.TotalLayer") == 0);
  CHECK(num("Status.PrintInfo.CurrentTicks") == 0 && num("Status.PrintInfo.TotalTicks") == 0);
  CHECK(is_null(jpath(&d, "Status.GKBridge.Z")));
  CHECK(jstr(&d, jpath(&d, "Status.GKBridge.Message"), s, sizeof s) == 0 && s[0] == 0);
  CHECK(jstr(&d, jpath(&d, "Status.PrintInfo.Filename"), s, sizeof s) == 0 && s[0] == 0);
  CHECK(count_nulls() == 1);

  // Z available and homed: no null anywhere
  hookw_parse_reply("{\"ok\":true,\"state\":\"idle\",\"z\":{\"pos\":1.25,\"homed\":true,\"moving\":true}}", &h);
  CHECK(frame(&h, &b, 0) == 0);
  CHECK(strstr(out, "\"Z\":{\"Pos\":1.25,\"Homed\":true,\"Moving\":true}") != NULL);
  CHECK(count_nulls() == 0);

  // Message precedence: hook message, bridge message, hook error
  hookw_parse_reply("{\"ok\":true,\"state\":\"idle\",\"message\":\"file check failed\"}", &h);
  snprintf(b.message, sizeof b.message, "bridge msg");
  CHECK(frame(&h, &b, 0) == 0);
  CHECK(jstr(&d, jpath(&d, "Status.GKBridge.Message"), s, sizeof s) == 0 && strcmp(s, "file check failed") == 0);
  idle_hk(&h);
  CHECK(frame(&h, &b, 0) == 0);
  CHECK(jstr(&d, jpath(&d, "Status.GKBridge.Message"), s, sizeof s) == 0 && strcmp(s, "bridge msg") == 0);
  hookw_parse_reply("{\"ok\":false,\"error\":\"hook not running\"}", &h);
  b.message[0] = 0;
  CHECK(frame(&h, &b, 0) == 0);
  CHECK(jstr(&d, jpath(&d, "Status.GKBridge.Message"), s, sizeof s) == 0 && strcmp(s, "hook: hook not running") == 0);
  CHECK(strstr(out, "\"HookOk\":false") != NULL);
  idle_hk(&h);
  CHECK(frame(&h, &b, 0) == 0);
  CHECK(strstr(out, "\"GKBridge\":{\"HookOk\":true,\"Z\":") != NULL);
  CHECK(num("Status.PrintInfo.Status") == 0);

  // ---- address change of the interface ----
  status_init("/tmp", "lo");
  CHECK(status_ip_changed() == 1);              // "" -> 127.0.0.1
  CHECK(status_ip_changed() == 0);
  status_init("/tmp", "nonexistent0");
  CHECK(status_ip_changed() == 1);              // the address is gone
  CHECK(status_ip_changed() == 0);

  // ---- attributes frame ----
  status_init("/tmp", "nonexistent0");
  idle_hk(&h);
  CHECK(frame(&h, &b, 1) == 0);
  CHECK(jstr(&d, jpath(&d, "Topic"), s, sizeof s) == 0 && strcmp(s, "sdcp/attributes/0000aabbccddeeff") == 0);
  CHECK(jstr(&d, jpath(&d, "Attributes.Resolution"), s, sizeof s) == 0 && strcmp(s, "7680x4320") == 0);
  CHECK(jstr(&d, jpath(&d, "Attributes.XYZsize"), s, sizeof s) == 0 && strcmp(s, "228.1x128.3x245") == 0);
  CHECK(jstr(&d, jpath(&d, "Attributes.MainboardIP"), s, sizeof s) == 0 && s[0] == 0);
  CHECK(jstr(&d, jpath(&d, "Attributes.ProtocolVersion"), s, sizeof s) == 0 && strcmp(s, "V3.0.0") == 0);
  CHECK(jstr(&d, jpath(&d, "Attributes.NetworkStatus"), s, sizeof s) == 0 && strcmp(s, "wlan") == 0);
  CHECK(jarr_n(&d, jpath(&d, "Attributes.Capabilities")) == 2 && jarr_n(&d, jpath(&d, "Attributes.SupportFileType")) == 1);
  CHECK(num("Attributes.DevicesStatus.ZMotorStatus") == 0 && num("Attributes.DevicesStatus.LCDStatus") == 1);
  CHECK(num("Attributes.RemainingMemory") > 0);
  CHECK(count_nulls() == 0);
  hookw_parse_reply("{\"ok\":true,\"state\":\"idle\",\"z\":{\"pos\":null,\"homed\":false,\"moving\":false},\"screen\":[1920,1080]}", &h);
  CHECK(frame(&h, &b, 1) == 0);
  CHECK(jstr(&d, jpath(&d, "Attributes.Resolution"), s, sizeof s) == 0 && strcmp(s, "1920x1080") == 0);
  CHECK(num("Attributes.DevicesStatus.ZMotorStatus") == 1);

  // ---- escapes in "file" ----
  hookw_parse_reply("{\"ok\":true,\"state\":\"idle\",\"file\":\"/media/emmc/a\\\"b.ctb\"}", &h);
  CHECK(h.hook_ok == 1 && strcmp(h.file, "/media/emmc/a\"b.ctb") == 0);
  hookw_parse_reply("{\"ok\":true,\"state\":\"idle\",\"file\":\"/media/emmc/a\\b.ctb\"}", &h);
  CHECK(h.hook_ok == 1 && strcmp(h.file, "/media/emmc/a\b.ctb") == 0);

  // ---- TaskId while a usb copy runs ----
  memset(&b, 0, sizeof b);
  snprintf(b.task_id, sizeof b.task_id, "0123456789abcdef0123456789abcdef");
  b.usb_copying = 1;
  idle_hk(&h);
  CHECK(frame(&h, &b, 0) == 0);
  CHECK(jstr(&d, jpath(&d, "Status.PrintInfo.TaskId"), s, sizeof s) == 0 && s[0] != 0);
  memset(&b, 0, sizeof b);

  // ---- prev_status / sub_status tracking (fresh boot first) ----
  with_state(&h, "idle", "");
  status_track(&h);
  CHECK(g_bridge.prev_status == 0 && g_bridge.sub_status == 0 && status_sub(&h, &g_bridge) == 0);
  with_state(&h, "printing", "exposing");
  status_track(&h);
  CHECK(g_bridge.prev_status == 0 && status_current(&h, &g_bridge) == 1);
  with_state(&h, "idle", "");                   // stopped: gkhook goes straight back to idle
  status_track(&h);
  CHECK(g_bridge.prev_status == 1 && g_bridge.sub_status == 8 && status_sub(&h, &g_bridge) == 8);
  with_state(&h, "starting", "");
  status_track(&h);
  with_state(&h, "printing", "homing");
  status_track(&h);
  with_state(&h, "complete", "");
  status_track(&h);
  CHECK(g_bridge.prev_status == 1 && g_bridge.sub_status == 9 && status_sub(&h, &g_bridge) == 9);
  with_state(&h, "idle", "");                   // dismissed: complete is remembered, not turned into 8
  status_track(&h);
  CHECK(g_bridge.prev_status == 1 && g_bridge.sub_status == 9 && status_sub(&h, &g_bridge) == 9);
  with_state(&h, "printing", "exposing");
  status_track(&h);
  with_state(&h, "error", "");
  status_track(&h);
  CHECK(g_bridge.sub_status == 8);
  with_state(&h, "idle", "");
  status_track(&h);
  CHECK(g_bridge.sub_status == 8);
  memset(&g_bridge, 0, sizeof g_bridge);
  return 0;
}
