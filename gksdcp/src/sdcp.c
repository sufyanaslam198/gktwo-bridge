#include "sdcp.h"
#include "clock.h"
#include "hookw.h"
#include "ident.h"
#include "loop.h"
#include "status.h"
#include "usbcopy.h"
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define MAX_CMDS     32
#define MAX_TOKENS   512
#define PUSH_MIN_MS  500
#define FRAME_CAP    16384

static struct { int cmd; sdcp_fn fn; } cmds[MAX_CMDS];
static int n_cmds;

void sdcp_register(int cmd, sdcp_fn fn) {
  for (int i = 0; i < n_cmds; i++)
    if (cmds[i].cmd == cmd) { cmds[i].fn = fn; return; }
  if (n_cmds < MAX_CMDS) { cmds[n_cmds].cmd = cmd; cmds[n_cmds].fn = fn; n_cmds++; }
}

static long long now_ms(void) {
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return (long long)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

// Send to one connection (conn != 0, ignored when closed) or to all.
static void send_to(uint32_t conn, const char *s, size_t n) {
  if (!conn) { ws_broadcast_text(s, n); return; }
  struct conn *c = conn_by_id(conn);
  if (c) conn_ws_text(c, s, n);
}

// ---- response, notice, error frames ----

void sdcp_reply(uint32_t conn, int cmd, const char *rid, int ack, const char *extra) {
  if (!conn_by_id(conn)) return;
  size_t cap = 2048 + strlen(extra);        // RequestID of 256 bytes can escape to 6x
  char *buf = malloc(cap);
  if (!buf) return;
  char topic[64];
  const char *id = ident_mainboard_id();
  struct jw w;
  jw_init(&w, buf, cap);
  jw_raw(&w, "{\"Id\":"); jw_str(&w, GK_BRAND_ID);
  jw_raw(&w, ",\"Data\":{\"Cmd\":"); jw_int(&w, cmd);
  jw_raw(&w, ",\"Data\":{\"Ack\":"); jw_int(&w, ack);
  jw_raw(&w, extra);
  jw_raw(&w, "},\"RequestID\":"); jw_str(&w, rid);
  jw_raw(&w, ",\"MainboardID\":"); jw_str(&w, id);
  jw_raw(&w, ",\"TimeStamp\":"); jw_int(&w, clock_now());
  snprintf(topic, sizeof topic, "sdcp/response/%s", id);
  jw_raw(&w, "},\"Topic\":"); jw_str(&w, topic);
  jw_raw(&w, "}");
  if (!w.err) send_to(conn, buf, w.len);
  free(buf);
}

// {"Id":..,"Data":{"Data":{<inner>},"MainboardID":..,"TimeStamp":..},"Topic":"sdcp/<kind>/<id>"}
static void send_event(const char *kind, const char *inner_key, const char *str_val, int int_val, int type) {
  size_t cap = 1024 + (str_val ? strlen(str_val) * 6 : 0);
  char *buf = malloc(cap);
  if (!buf) return;
  char topic[64];
  const char *id = ident_mainboard_id();
  struct jw w;
  jw_init(&w, buf, cap);
  jw_raw(&w, "{\"Id\":"); jw_str(&w, GK_BRAND_ID);
  jw_raw(&w, ",\"Data\":{\"Data\":{\""); jw_raw(&w, inner_key); jw_raw(&w, "\":");
  if (str_val) jw_str(&w, str_val); else jw_int(&w, int_val);
  if (type >= 0) { jw_raw(&w, ",\"Type\":"); jw_int(&w, type); }
  jw_raw(&w, "},\"MainboardID\":"); jw_str(&w, id);
  jw_raw(&w, ",\"TimeStamp\":"); jw_int(&w, clock_now());
  snprintf(topic, sizeof topic, "sdcp/%s/%s", kind, id);
  jw_raw(&w, "},\"Topic\":"); jw_str(&w, topic);
  jw_raw(&w, "}");
  if (!w.err) ws_broadcast_text(buf, w.len);
  free(buf);
}

void sdcp_notice(const char *msg) { send_event("notice", "Message", msg, 0, 100); }
void sdcp_error(int code) { send_event("error", "ErrorCode", NULL, code, -1); }

// ---- status and attributes pushes ----

enum { K_STATUS, K_ATTR, K_N };
static long long last_push[K_N];
static int pending[K_N];

static void push_status_now(uint32_t conn) {
  static char buf[FRAME_CAP];
  struct hk_status h;
  struct jw w;
  hookw_status(&h);
  jw_init(&w, buf, sizeof buf);
  status_write(&w, &h, &g_bridge);
  if (!w.err) send_to(conn, buf, w.len);
}

static void push_attr_now(uint32_t conn) {
  static char buf[4096];
  struct hk_status h;
  struct jw w;
  hookw_status(&h);
  jw_init(&w, buf, sizeof buf);
  attributes_write(&w, &h);
  if (!w.err) send_to(conn, buf, w.len);
}

static void push_kind(int kind, uint32_t conn) {
  if (!conn) {                             // a unicast reply does not count against the broadcast rate
    last_push[kind] = now_ms();
    pending[kind] = 0;
  }
  if (kind == K_STATUS) push_status_now(conn); else push_attr_now(conn);
}

void sdcp_push_status(uint32_t conn) { push_kind(K_STATUS, conn); }
void sdcp_push_attributes(uint32_t conn) { push_kind(K_ATTR, conn); }

static void dirty(int kind) {
  if (kind == K_STATUS) {                  // every change is tracked, whether or not it is pushed now
    struct hk_status h;
    hookw_status(&h);
    status_track(&h);
  }
  if (now_ms() - last_push[kind] >= PUSH_MIN_MS) push_kind(kind, 0);
  else pending[kind] = 1;                  // the timer sends the latest state when the interval is up
}

void sdcp_status_dirty(void) { dirty(K_STATUS); }
void sdcp_attributes_dirty(void) { dirty(K_ATTR); }

static void flush_pending(void) {
  for (int k = 0; k < K_N; k++)
    if (pending[k] && now_ms() - last_push[k] >= PUSH_MIN_MS) push_kind(k, 0);
}

// MainboardIP is in the attributes: a new address (DHCP, another network) is pushed like UsbDiskStatus.
static void ip_poll(void) {
  if (status_ip_changed()) sdcp_attributes_dirty();
}

// A new client gets the attributes, then the status, without asking. Clients that cannot use UDP discovery
// (another subnet) learn the MainboardID from these frames: DragonFruit sends "ping" and reads for ~1.8 s.
static void on_open(struct conn *c) {
  sdcp_push_attributes(conn_id(c));
  sdcp_push_status(conn_id(c));
}

// ---- requests ----

static void cmd_status(struct sdcp_req *r) {
  sdcp_reply(r->conn, r->cmd, r->rid, 0, "");
  sdcp_push_status(r->conn);
}

static void cmd_attributes(struct sdcp_req *r) {
  sdcp_reply(r->conn, r->cmd, r->rid, 0, "");
  sdcp_push_attributes(r->conn);
}

// Log a value once per distinct (kind, value); stops logging when the table is full.
static void log_once(const char *kind, const char *val, uint32_t conn) {
  static char seen[16][96];
  char key[96];
  snprintf(key, sizeof key, "%s=%s", kind, val);
  for (int i = 0; i < 16; i++) {
    if (!seen[i][0]) {
      memcpy(seen[i], key, sizeof key);
      fprintf(stderr, "gksdcp: conn %u: request %s (answered anyway)\n", conn, key);
      return;
    }
    if (strcmp(seen[i], key) == 0) return;
  }
}

// Requests are never rejected for a wrong MainboardID or Topic, only logged.
static void check_addressing(const struct jdoc *d, uint32_t conn) {
  char v[48], want[64];
  const char *id = ident_mainboard_id();
  int t = jpath(d, "Data.MainboardID");
  if (t >= 0) {
    if (jstr(d, t, v, sizeof v) < 0) snprintf(v, sizeof v, "(unreadable)");
    if (strcmp(v, id) != 0) log_once("MainboardID", v, conn);
  }
  t = jpath(d, "Topic");
  if (t >= 0) {
    snprintf(want, sizeof want, "sdcp/request/%s", id);
    char tp[48];
    if (jstr(d, t, tp, sizeof tp) < 0) snprintf(tp, sizeof tp, "(unreadable)");
    if (strcmp(tp, want) != 0) log_once("Topic", tp, conn);
  }
}

static void on_request(struct conn *c, struct jdoc d) {
  if (d.n < 1 || d.t[0].type != JSMN_OBJECT) return;
  long long cmd, ts;
  int t = jpath(&d, "Data.Cmd");
  if (t < 0 || jint(&d, t, &cmd) < 0 || cmd < INT_MIN || cmd > INT_MAX) return;

  struct sdcp_req r = { .conn = conn_id(c), .cmd = (int)cmd, .d = &d, .data = jpath(&d, "Data.Data") };
  t = jpath(&d, "Data.RequestID");                  // missing, not a string or over 256 bytes: no reply
  if (t < 0 || jstr(&d, t, r.rid, sizeof r.rid) < 0) return;
  t = jpath(&d, "Data.TimeStamp");
  if (t >= 0 && jint(&d, t, &ts) == 0) clock_learn(ts);
  check_addressing(&d, r.conn);

  for (int i = 0; i < n_cmds; i++)
    if (cmds[i].cmd == r.cmd) { cmds[i].fn(&r); return; }
  sdcp_reply(r.conn, r.cmd, r.rid, 1, "");
}

// Most requests fit the static tokens. A long array (Cmd 259 paths, Cmd 321 ids) gets a buffer of the exact
// size for this request only; the frame limit (MAX_WS_PAYLOAD) bounds it.
void sdcp_on_text(struct conn *c, const char *s, size_t n) {
  static jsmntok_t toks[MAX_TOKENS];
  struct jdoc d;
  if (jdoc_parse(&d, s, n, toks, MAX_TOKENS) == 0) { on_request(c, d); return; }
  int need = jdoc_count(s, n);
  if (need <= MAX_TOKENS) return;                   // not valid JSON
  jsmntok_t *big = malloc((size_t)need * sizeof *big);
  if (big && jdoc_parse(&d, s, n, big, need) == 0) on_request(c, d);
  free(big);
}

// Debug builds only, for tests: broadcast from on_conn_close.
#ifndef NDEBUG
static void (*prev_on_close)(struct conn *);
static void test_on_close(struct conn *c) {
  if (prev_on_close) prev_on_close(c);
  sdcp_notice("closed");
}
#endif

void sdcp_init(void) {
  on_ws_text = sdcp_on_text;
  on_ws_open = on_open;
  sdcp_register(0, cmd_status);
  sdcp_register(1, cmd_attributes);
  cmd_print_init();
  cmd_files_init();
  cmd_z_init();
  usbcopy_init();
  if (hookw_on_change(sdcp_status_dirty) < 0) fprintf(stderr, "gksdcp: no free hook change slot\n");
  loop_every(100, flush_pending);
  status_ip_changed();                     // the address at start-up is no change
  loop_every(2000, ip_poll);
#ifndef NDEBUG
  if (getenv("GKSDCP_TEST_CLOSE_NOTICE")) { prev_on_close = on_conn_close; on_conn_close = test_on_close; }
#endif
}
