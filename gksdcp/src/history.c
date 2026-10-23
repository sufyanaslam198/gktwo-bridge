#include "history.h"
#include "clock.h"
#include "ident.h"
#include "sdcp.h"
#include "status.h"
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#define NAME_MAX_B   128        // TaskName, NUL included
#define FILE_MAX_B   256        // hook file path kept for recovery, NUL included
#define FILE_LIMIT   (256 * 1024)
#define LOAD_TOKENS  2048
#define REC_JSON_MAX 4096       // one record, worst case with every byte escaped
#define DETAIL_PER   1200       // Cmd 321 output budget per record

struct rec {
  char id[37];
  char name[NAME_MAX_B];
  char file[FILE_MAX_B];      // the hook's path of the print; "" until the hook reports it
  long long begin, end;       // end 0 while open
  int status;                 // 0 open, 1 completed, 2 error, 3 stopped
  long layers;
  int reason;
};

static struct rec recs[HISTORY_CAP];   // oldest first
static int n_recs, g_cap = HISTORY_CAP;
static char g_path[300];
static int g_recovered;                // history_recover has seen a reachable hook
static char prev_state[16] = "idle";   // state at the last snapshot tracked
static long max_layer;                 // highest layer seen during the open task
// What history_begin replaced, so history_abort can put it back.
static char begun_id[37], saved_task_id[37], saved_client_file[600];

// ---- helpers ----

// Copy at most cap-1 bytes, never splitting a UTF-8 sequence.
static void copy_utf8(char *dst, size_t cap, const char *src) {
  size_t n = strlen(src);
  if (n > cap - 1) {
    n = cap - 1;
    while (n > 0 && ((unsigned char)src[n] & 0xc0) == 0x80) n--;
  }
  memcpy(dst, src, n);
  dst[n] = 0;
}

static int is_run(const char *s) {
  return strcmp(s, "starting") == 0 || strcmp(s, "printing") == 0 || strcmp(s, "paused") == 0;
}

static const char *base_name(const char *path) {
  const char *b = strrchr(path, '/');
  return b ? b + 1 : path;
}

static struct rec *open_rec(void) {
  return (n_recs && recs[n_recs - 1].end == 0) ? &recs[n_recs - 1] : NULL;
}

void history_new_id(char out[37]) {
  unsigned char b[16];
  int ok = 0;
  int fd = open("/dev/urandom", O_RDONLY | O_CLOEXEC);
  if (fd >= 0) {
    ok = read(fd, b, sizeof b) == (ssize_t)sizeof b;
    close(fd);
  }
  if (!ok) {                             // splitmix64 over the time, the pid and a counter
    static uint64_t counter;
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    uint64_t x = (uint64_t)ts.tv_sec * 1000000007ull + (uint64_t)ts.tv_nsec + ((uint64_t)getpid() << 32) +
                 ++counter * 0x9e3779b97f4a7c15ull;
    for (int i = 0; i < 16; i += 8) {
      x += 0x9e3779b97f4a7c15ull;
      uint64_t z = x;
      z = (z ^ (z >> 30)) * 0xbf58476d1ce4e5b9ull;
      z = (z ^ (z >> 27)) * 0x94d049bb133111ebull;
      z ^= z >> 31;
      for (int k = 0; k < 8; k++) b[i + k] = (unsigned char)(z >> (8 * k));
    }
  }
  b[6] = (unsigned char)((b[6] & 0x0f) | 0x40);
  b[8] = (unsigned char)((b[8] & 0x3f) | 0x80);
  snprintf(out, 37, "%02x%02x%02x%02x-%02x%02x-%02x%02x-%02x%02x-%02x%02x%02x%02x%02x%02x",
           b[0], b[1], b[2], b[3], b[4], b[5], b[6], b[7], b[8], b[9], b[10], b[11], b[12], b[13], b[14], b[15]);
}

// ---- JSON of one record: the file entry (persist=1, adds File) and the Cmd 321 item ----

static void rec_write(struct jw *w, const struct rec *r, int persist) {
  jw_raw(w, "{\"Thumbnail\":\"\",\"TaskName\":"); jw_str(w, r->name);
  jw_raw(w, ",\"BeginTime\":"); jw_int(w, r->begin);
  jw_raw(w, ",\"EndTime\":"); jw_int(w, r->end);
  jw_raw(w, ",\"TaskStatus\":"); jw_int(w, r->status);
  jw_raw(w, ",\"SliceInformation\":{},\"AlreadyPrintLayer\":"); jw_int(w, r->layers);
  jw_raw(w, ",\"TaskId\":"); jw_str(w, r->id);
  jw_raw(w, ",\"MD5\":\"\",\"CurrentLayerTalVolume\":0,\"TimeLapseVideoStatus\":0,\"TimeLapseVideoUrl\":\"\","
            "\"ErrorStatusReason\":");
  jw_int(w, r->reason);
  if (persist) { jw_raw(w, ",\"File\":"); jw_str(w, r->file); }
  jw_raw(w, "}");
}

// ---- persistence ----

// Failures are logged here (every caller keeps its record in RAM and carries on).
static int save(void) {
  char tmp[sizeof g_path + 8], dir[sizeof g_path];
  snprintf(tmp, sizeof tmp, "%s.tmp", g_path);
  snprintf(dir, sizeof dir, "%s", g_path);
  char *slash = strrchr(dir, '/');
  if (slash && slash != dir) { *slash = 0; ident_mkdir_p(dir); }
  FILE *f = fopen(tmp, "w");
  if (!f) { fprintf(stderr, "gksdcp: cannot write %s: %s\n", tmp, strerror(errno)); return -1; }
  static char buf[REC_JSON_MAX];
  int ok = fputs("{\"Version\":1,\"Tasks\":[", f) >= 0;
  for (int i = 0; ok && i < n_recs; i++) {
    struct jw w;
    jw_init(&w, buf, sizeof buf);
    rec_write(&w, &recs[i], 1);
    ok = !w.err && (i == 0 || fputc(',', f) >= 0) && fwrite(buf, 1, w.len, f) == w.len;
  }
  ok = ok && fputs("]}\n", f) >= 0 && fflush(f) == 0 && fsync(fileno(f)) == 0;
  if (fclose(f) != 0) ok = 0;
  if (!ok || rename(tmp, g_path) < 0) {
    fprintf(stderr, "gksdcp: cannot save %s\n", g_path);
    unlink(tmp);
    return -1;
  }
  return 0;
}

static long long num(const struct jdoc *d, int obj, const char *key) {
  long long v;
  int t = jget(d, obj, key);
  return (t >= 0 && jint(d, t, &v) == 0) ? v : 0;
}

static void text(const struct jdoc *d, int obj, const char *key, char *out, size_t cap) {
  char tmp[REC_JSON_MAX];
  int t = jget(d, obj, key);
  out[0] = 0;
  if (t >= 0 && jstr(d, t, tmp, sizeof tmp) == 0) copy_utf8(out, cap, tmp);
}

// Parses the file text into recs. 0 ok / -1 corrupt.
static int parse_file(const char *js, size_t len) {
  jsmntok_t *toks = malloc(LOAD_TOKENS * sizeof *toks);
  if (!toks) return -1;
  struct jdoc d;
  int rc = -1;
  if (jdoc_parse(&d, js, len, toks, LOAD_TOKENS) < 0 || d.n < 1 || d.t[0].type != JSMN_OBJECT) goto out;
  int arr = jget(&d, 0, "Tasks");
  int n = jarr_n(&d, arr);
  if (n < 0) goto out;
  int skip = n > g_cap ? n - g_cap : 0;
  n_recs = 0;
  for (int i = skip; i < n; i++) {
    int o = jarr_at(&d, arr, i);
    struct rec *r = &recs[n_recs];
    memset(r, 0, sizeof *r);
    int t = jget(&d, o, "TaskId");
    char id[64];
    if (t < 0 || jstr(&d, t, id, sizeof id) < 0 || strlen(id) != 36) { n_recs = 0; goto out; }
    memcpy(r->id, id, 37);
    text(&d, o, "TaskName", r->name, sizeof r->name);
    text(&d, o, "File", r->file, sizeof r->file);
    r->begin = num(&d, o, "BeginTime");
    r->end = num(&d, o, "EndTime");
    r->status = (int)num(&d, o, "TaskStatus");
    r->layers = (long)num(&d, o, "AlreadyPrintLayer");
    r->reason = (int)num(&d, o, "ErrorStatusReason");
    n_recs++;
  }
  rc = 0;
out:
  free(toks);
  return rc;
}

int history_init(const char *path, int cap) {
  if (!path || cap < 1 || cap > HISTORY_CAP || strlen(path) >= sizeof g_path - 8) return -1;
  snprintf(g_path, sizeof g_path, "%s", path);
  g_cap = cap;
  n_recs = 0;
  g_recovered = 0;
  strcpy(prev_state, "idle");
  max_layer = 0;
  begun_id[0] = 0;

  FILE *f = fopen(path, "r");
  if (!f) return 0;                       // missing: start empty
  char *js = malloc(FILE_LIMIT + 1);
  size_t len = js ? fread(js, 1, FILE_LIMIT + 1, f) : 0;
  int rd_err = ferror(f);
  fclose(f);
  if (js && !rd_err && len <= FILE_LIMIT && parse_file(js, len) == 0) {
    for (int i = 0; i + 1 < n_recs; i++)  // only the newest may be open
      if (recs[i].end == 0) { recs[i].end = recs[i].begin ? recs[i].begin : 1; recs[i].status = 3; }
    free(js);
    return 0;
  }
  free(js);
  n_recs = 0;
  char bad[sizeof g_path + 8];
  snprintf(bad, sizeof bad, "%s.bad", path);
  fprintf(stderr, "gksdcp: %s is corrupt, moved to %s, starting with empty history\n", path, bad);
  rename(path, bad);
  return 0;
}

// ---- tasks ----

static const char *begin_with(const char *task_name, const char *client_file, const char *hook_file) {
  struct rec *o = open_rec();
  if (o) {                                // a task still open is over
    o->end = clock_now();
    o->status = 3;
  }
  if (n_recs == g_cap) {
    memmove(&recs[0], &recs[1], (size_t)(g_cap - 1) * sizeof recs[0]);
    n_recs--;
  }
  struct rec *r = &recs[n_recs++];
  memset(r, 0, sizeof *r);
  history_new_id(r->id);
  copy_utf8(r->name, sizeof r->name, task_name ? task_name : "");
  copy_utf8(r->file, sizeof r->file, hook_file ? hook_file : "");
  r->begin = clock_now();
  max_layer = 0;
  snprintf(g_bridge.task_id, sizeof g_bridge.task_id, "%s", r->id);
  snprintf(g_bridge.client_file, sizeof g_bridge.client_file, "%s", client_file ? client_file : "");
  save();
  return r->id;
}

const char *history_begin(const char *task_name, const char *client_file) {
  struct hk_status h;
  hookw_status(&h);
  if (open_rec() && (is_run(prev_state) || is_run(h.state))) return NULL;   // a live print owns the task
  char old_id[37], old_file[sizeof saved_client_file];
  snprintf(old_id, sizeof old_id, "%s", g_bridge.task_id);
  snprintf(old_file, sizeof old_file, "%s", g_bridge.client_file);
  const char *id = begin_with(task_name, client_file, NULL);
  memcpy(saved_task_id, old_id, sizeof old_id);
  memcpy(saved_client_file, old_file, sizeof old_file);
  memcpy(begun_id, id, 37);
  return id;
}

void history_abort(void) {
  struct rec *r = open_rec();
  if (!r || !begun_id[0] || strcmp(r->id, begun_id) != 0 || r->file[0]) return;   // the hook started it
  n_recs--;
  begun_id[0] = 0;
  snprintf(g_bridge.task_id, sizeof g_bridge.task_id, "%s", saved_task_id);
  snprintf(g_bridge.client_file, sizeof g_bridge.client_file, "%s", saved_client_file);
  save();
}

void history_adopt(const struct hk_status *h) {
  struct rec *r = open_rec();
  if (!r) return;
  copy_utf8(r->name, sizeof r->name, base_name(h->file));
  copy_utf8(r->file, sizeof r->file, h->file);
  begun_id[0] = 0;
  save();
}

void history_end(int task_status, long layers, int reason) {
  struct rec *r = open_rec();
  if (!r) return;
  r->end = clock_now();
  r->status = task_status;
  r->layers = layers > 0 ? layers : 0;
  r->reason = reason;
  save();
}

void history_end_begun(void) {
  struct rec *r = open_rec();
  if (r && begun_id[0] && strcmp(r->id, begun_id) == 0) history_end(3, 0, 0);
}

static long layers_now(const struct hk_status *h) {
  return h->layer > max_layer ? h->layer : max_layer;
}

void history_track(const struct hk_status *h) {
  // An unreachable hook reads as idle: that says nothing about the print, so it is not tracked.
  if (!h->hook_ok) return;
  if (!g_recovered) { history_recover(h); return; }
  struct rec *o = open_rec();
  int run = is_run(h->state);
  int changed = strcmp(prev_state, h->state) != 0;
  if (o && run) {
    if (h->layer > max_layer) max_layer = h->layer;
    if (!o->file[0] && h->file[0]) {      // started by the bridge: the hook's path is known only now
      copy_utf8(o->file, sizeof o->file, h->file);
      save();
    }
  }
  if (o && changed && strcmp(h->state, "complete") == 0) {
    long l = layers_now(h);
    history_end(1, h->total_layers > l ? h->total_layers : l, 0);
  } else if (o && changed && strcmp(h->state, "error") == 0) {
    history_end(2, layers_now(h), 19);
  } else if (o && changed && strcmp(h->state, "idle") == 0 && is_run(prev_state)) {
    history_end(3, layers_now(h), 0);
  } else if (!o && run && !is_run(prev_state)) {   // a print started from the touchscreen
    begin_with(base_name(h->file), NULL, h->file);
    if (h->layer > max_layer) max_layer = h->layer;
  }
  snprintf(prev_state, sizeof prev_state, "%s", h->state);
}

// Runs once, on the first snapshot that comes from a reachable hook: until then nothing is known about
// the printer, so an open record is left alone.
void history_recover(const struct hk_status *h) {
  if (!h->hook_ok) return;
  g_recovered = 1;
  struct rec *o = open_rec();
  char file[FILE_MAX_B];
  copy_utf8(file, sizeof file, h->file);
  // The path is known once the hook reported it; a bridge that died before that matches on the task name.
  int same = o && (o->file[0] ? strcmp(o->file, file) == 0 : strcmp(o->name, base_name(file)) == 0);
  if (same && is_run(h->state)) {
    snprintf(g_bridge.task_id, sizeof g_bridge.task_id, "%s", o->id);   // the print carries on
    g_bridge.client_file[0] = 0;
    max_layer = h->layer > 0 ? h->layer : 0;
  } else {
    if (o) history_end(3, o->layers, 0);
    if (is_run(h->state)) begin_with(base_name(h->file), NULL, h->file);
  }
  snprintf(prev_state, sizeof prev_state, "%s", h->state);
}

// ---- Cmd 320 / 321 ----

void history_ids(struct jw *w) {
  jw_raw(w, "[");
  for (int i = n_recs - 1; i >= 0; i--) {
    if (i != n_recs - 1) jw_raw(w, ",");
    jw_str(w, recs[i].id);
  }
  jw_raw(w, "]");
}

void history_details(struct jw *w, const struct jdoc *d, int ids_tok) {
  jw_raw(w, "[");
  int n = jarr_n(d, ids_tok);
  unsigned long long seen = 0;
  int first = 1;
  for (int i = 0, e = ids_tok + 1; i < n; i++, e = jnext(d, e)) {
    char id[64];
    if (jstr(d, e, id, sizeof id) < 0) continue;
    for (int k = 0; k < n_recs; k++) {
      if (strcmp(recs[k].id, id) != 0 || (seen >> k & 1)) continue;
      seen |= 1ull << k;
      if (!first) jw_raw(w, ",");
      first = 0;
      rec_write(w, &recs[k], 0);
    }
  }
  jw_raw(w, "]");
}

static void cmd_history_ids(struct sdcp_req *r) {
  char buf[HISTORY_CAP * 40 + 64];
  struct jw w;
  jw_init(&w, buf, sizeof buf);
  jw_raw(&w, ",\"HistoryData\":");
  history_ids(&w);
  sdcp_reply(r->conn, r->cmd, r->rid, w.err ? 1 : 0, w.err ? "" : buf);
}

static void cmd_history_details(struct sdcp_req *r) {
  // DragonFruit sends the same list as both Id and TaskIdList; the first key holding an array is used.
  int ids = jget(r->d, r->data, "Id");
  if (jarr_n(r->d, ids) < 0) ids = jget(r->d, r->data, "TaskIdList");
  if (jarr_n(r->d, ids) < 0) {
    sdcp_reply(r->conn, r->cmd, r->rid, 1, ",\"HistoryDetailList\":[]");
    return;
  }
  size_t cap = (size_t)n_recs * DETAIL_PER + 128;
  char *buf = malloc(cap);
  if (!buf) {
    sdcp_reply(r->conn, r->cmd, r->rid, 1, ",\"HistoryDetailList\":[]");
    return;
  }
  struct jw w;
  jw_init(&w, buf, cap);
  jw_raw(&w, ",\"HistoryDetailList\":");
  history_details(&w, r->d, ids);
  if (w.err) sdcp_reply(r->conn, r->cmd, r->rid, 1, ",\"HistoryDetailList\":[]");
  else sdcp_reply(r->conn, r->cmd, r->rid, 0, buf);
  free(buf);
}

static void on_change(void) {
  struct hk_status h;
  hookw_status(&h);
  history_track(&h);
}

void history_attach(void) {
  struct hk_status h;
  hookw_status(&h);
  history_recover(&h);
  if (hookw_on_change(on_change) < 0) fprintf(stderr, "gksdcp: no free hook change slot for history\n");
  sdcp_register(320, cmd_history_ids);
  sdcp_register(321, cmd_history_details);
}
