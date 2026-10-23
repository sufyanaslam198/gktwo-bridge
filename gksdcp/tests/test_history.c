#include "clock.h"
#include "history.h"
#include "ident.h"
#include "json.h"
#include "status.h"
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define CHECK(c) do { if (!(c)) { fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #c); exit(1); } } while (0)

static char path[256], dir[128];

static void hk(struct hk_status *h, const char *state, const char *file, long layer, long total) {
  memset(h, 0, sizeof *h);
  h->hook_ok = 1;
  snprintf(h->state, sizeof h->state, "%s", state);
  snprintf(h->file, sizeof h->file, "%s", file);
  h->layer = layer;
  h->total_layers = total;
  h->elapsed_s = h->remaining_s = -1;
}

// TaskIds, newest first, into out
static int ids(char out[][37], int max) {
  static char buf[8192];
  static jsmntok_t t[256];
  struct jw w;
  struct jdoc d;
  jw_init(&w, buf, sizeof buf);
  history_ids(&w);
  CHECK(!w.err && jdoc_parse(&d, buf, w.len, t, 256) == 0);
  int n = jarr_n(&d, 0);
  CHECK(n >= 0 && n <= max);
  for (int i = 0; i < n; i++) CHECK(jstr(&d, jarr_at(&d, 0, i), out[i], 37) == 0);
  return n;
}

static char dbuf[65536];
static jsmntok_t dtoks[2048];
static struct jdoc dd;

// details of the ids in a JSON array; returns the number of items, dd is the parsed output
static int details(const char *id_json) {
  static jsmntok_t it[64];
  struct jdoc idoc;
  struct jw w;
  CHECK(jdoc_parse(&idoc, id_json, strlen(id_json), it, 64) == 0);
  jw_init(&w, dbuf, sizeof dbuf);
  history_details(&w, &idoc, 0);
  CHECK(!w.err && jdoc_parse(&dd, dbuf, w.len, dtoks, 2048) == 0);
  return jarr_n(&dd, 0);
}

static long long fld(const char *key) {          // numeric field of item 0 of the last details()
  long long v;
  int t = jget(&dd, jarr_at(&dd, 0, 0), key);
  CHECK(t >= 0 && jint(&dd, t, &v) == 0);
  return v;
}

static void sfld(const char *key, char *out, size_t cap) {
  int t = jget(&dd, jarr_at(&dd, 0, 0), key);
  CHECK(t >= 0 && jstr(&dd, t, out, cap) == 0);
}

static int one(const char *id) {                 // details of one id into item 0; -1 when absent
  char j[64];
  snprintf(j, sizeof j, "[\"%s\"]", id);
  return details(j) == 1 ? 0 : -1;
}

static void test_uuid(void) {
  char a[37], b[37];
  history_new_id(a);
  history_new_id(b);
  CHECK(strlen(a) == 36 && strcmp(a, b) != 0);
  for (int i = 0; i < 36; i++) {
    if (i == 8 || i == 13 || i == 18 || i == 23) CHECK(a[i] == '-');
    else CHECK(isdigit((unsigned char)a[i]) || (a[i] >= 'a' && a[i] <= 'f'));
  }
  CHECK(a[14] == '4' && strchr("89ab", a[19]));
}

static void test_begin_end_reload_cap(void) {
  char l[HISTORY_CAP + 2][37];
  CHECK(history_init(path, 3) == 0);             // parent dir does not exist yet
  CHECK(ids(l, 8) == 0);
  const char *id = history_begin("a.ctb", "/local/a.ctb");
  CHECK(strlen(id) == 36 && strcmp(id, g_bridge.task_id) == 0);
  CHECK(strcmp(g_bridge.client_file, "/local/a.ctb") == 0);
  CHECK(ids(l, 8) == 1 && strcmp(l[0], id) == 0);
  CHECK(one(id) == 0 && fld("EndTime") == 0 && fld("TaskStatus") == 0 && fld("BeginTime") > 0);
  history_end(1, 42, 0);
  CHECK(one(id) == 0 && fld("EndTime") > 0 && fld("TaskStatus") == 1 && fld("AlreadyPrintLayer") == 42);
  char name[64];
  sfld("TaskName", name, sizeof name);
  CHECK(strcmp(name, "a.ctb") == 0);
  CHECK(fld("ErrorStatusReason") == 0 && fld("TimeLapseVideoStatus") == 0);
  history_end(2, 1, 19);                         // nothing open: ignored
  CHECK(one(id) == 0 && fld("TaskStatus") == 1);

  CHECK(history_init(path, 3) == 0);             // reload from disk
  CHECK(ids(l, 8) == 1 && strcmp(l[0], id) == 0);
  CHECK(one(id) == 0 && fld("EndTime") > 0 && fld("AlreadyPrintLayer") == 42);

  char first[37], newest[37];
  snprintf(first, sizeof first, "%s", id);
  for (int i = 0; i < 4; i++) {
    snprintf(newest, sizeof newest, "%s", history_begin("b.ctb", NULL));
    history_end(2, 5, 19);
  }
  CHECK(strcmp(g_bridge.client_file, "") == 0);
  CHECK(ids(l, 8) == 3 && strcmp(l[0], newest) == 0);     // oldest dropped, newest first
  CHECK(one(first) == -1);
  CHECK(one(newest) == 0 && fld("TaskStatus") == 2 && fld("ErrorStatusReason") == 19);

  CHECK(history_init(path, 2) == 0);             // smaller cap on reload keeps the newest
  CHECK(ids(l, 8) == 2 && strcmp(l[0], newest) == 0);
}

static void test_details_ids(void) {
  CHECK(history_init(path, 50) == 0);
  char l[4][37];
  CHECK(ids(l, 4) >= 1);
  char j[256];
  snprintf(j, sizeof j, "[\"%s\",\"nope\",\"%s\",7,null]", l[0], l[0]);
  CHECK(details(j) == 1);                        // unknown skipped, duplicate once
  CHECK(details("\"x\"") == 0 && strcmp(dbuf, "[]") == 0);   // not an array: []
  CHECK(details("{\"a\":1}") == 0 && strcmp(dbuf, "[]") == 0);
}

static void test_track(void) {
  char l[HISTORY_CAP + 2][37], id[37], nm[64];
  struct hk_status h;
  unlink(path);
  CHECK(history_init(path, 50) == 0);
  hk(&h, "idle", "", -1, -1);
  history_recover(&h);
  history_track(&h);
  CHECK(ids(l, 8) == 0);

  // touchscreen print: idle -> starting -> printing -> paused -> printing -> complete
  hk(&h, "starting", "/media/emmc/x.ctb", -1, -1); history_track(&h);
  CHECK(ids(l, 8) == 1);
  snprintf(id, sizeof id, "%s", l[0]);
  CHECK(strcmp(g_bridge.task_id, id) == 0 && g_bridge.client_file[0] == 0);
  hk(&h, "printing", "/media/emmc/x.ctb", 3, 10); history_track(&h);
  hk(&h, "paused", "/media/emmc/x.ctb", 5, 10); history_track(&h);
  hk(&h, "printing", "/media/emmc/x.ctb", 6, 10); history_track(&h);
  CHECK(ids(l, 8) == 1);
  CHECK(one(id) == 0 && fld("EndTime") == 0);
  sfld("TaskName", nm, sizeof nm);
  CHECK(strcmp(nm, "x.ctb") == 0);
  hk(&h, "complete", "/media/emmc/x.ctb", 10, 10); history_track(&h);
  CHECK(one(id) == 0 && fld("TaskStatus") == 1 && fld("EndTime") > 0 && fld("AlreadyPrintLayer") == 10);
  hk(&h, "idle", "", -1, -1); history_track(&h);
  CHECK(ids(l, 8) == 1);

  // error mid-print
  hk(&h, "printing", "/media/emmc/y.ctb", 2, 10); history_track(&h);
  CHECK(ids(l, 8) == 2);
  snprintf(id, sizeof id, "%s", l[0]);
  hk(&h, "error", "/media/emmc/y.ctb", 2, 10); history_track(&h);
  CHECK(one(id) == 0 && fld("TaskStatus") == 2 && fld("ErrorStatusReason") == 19 && fld("EndTime") > 0);
  hk(&h, "idle", "", -1, -1); history_track(&h);

  // stopped: printing -> idle
  hk(&h, "printing", "/media/emmc/z.ctb", 4, 10); history_track(&h);
  CHECK(ids(l, 8) == 3);
  snprintf(id, sizeof id, "%s", l[0]);
  hk(&h, "idle", "", -1, -1); history_track(&h);
  CHECK(one(id) == 0 && fld("TaskStatus") == 3 && fld("EndTime") > 0 && fld("AlreadyPrintLayer") == 4);

  // started by the bridge: an unrelated idle snapshot must not close it; a start that fails does
  snprintf(id, sizeof id, "%s", history_begin("w.ctb", "/local/w.ctb"));
  hk(&h, "idle", "", -1, -1); history_track(&h);
  CHECK(ids(l, 8) == 4 && one(id) == 0 && fld("EndTime") == 0);
  hk(&h, "starting", "/media/emmc/w.ctb", -1, -1); history_track(&h);
  CHECK(ids(l, 8) == 4);                         // no second task
  CHECK(strcmp(g_bridge.client_file, "/local/w.ctb") == 0);
  hk(&h, "idle", "", -1, -1);
  snprintf(h.message, sizeof h.message, "file check failed");
  history_track(&h);
  CHECK(one(id) == 0 && fld("TaskStatus") == 3 && fld("EndTime") > 0);
}

static void test_unreachable_hook_ignored(void) {
  char l[8][37], id[37];
  struct hk_status h;
  unlink(path);
  CHECK(history_init(path, 50) == 0);
  hk(&h, "printing", "/media/emmc/f.ctb", 2, 9);
  h.hook_ok = 0;                                 // hook down at start-up: nothing is decided
  history_recover(&h);
  history_track(&h);
  CHECK(ids(l, 8) == 0);
  h.hook_ok = 1; history_recover(&h);
  CHECK(ids(l, 8) == 1);
  snprintf(id, sizeof id, "%s", l[0]);
  hk(&h, "idle", "", -1, -1);                    // hook goes away mid-print: reads as idle, not a stop
  h.hook_ok = 0; history_track(&h);
  CHECK(one(id) == 0 && fld("EndTime") == 0);
  hk(&h, "printing", "/media/emmc/f.ctb", 3, 9); history_track(&h);
  CHECK(ids(l, 8) == 1);                         // and no second task when it comes back
}

static void test_begin_refused_while_printing_and_abort(void) {
  char l[8][37], id[37];
  struct hk_status h;
  unlink(path);
  CHECK(history_init(path, 50) == 0);
  hk(&h, "idle", "", -1, -1);
  history_recover(&h);
  hk(&h, "printing", "/media/emmc/live.ctb", 2, 9); history_track(&h);
  CHECK(ids(l, 8) == 1);
  snprintf(id, sizeof id, "%s", l[0]);
  CHECK(history_begin("other.ctb", "/local/other.ctb") == NULL);
  CHECK(ids(l, 8) == 1 && strcmp(l[0], id) == 0 && strcmp(g_bridge.task_id, id) == 0);
  CHECK(one(id) == 0 && fld("EndTime") == 0 && fld("TaskStatus") == 0);
  hk(&h, "complete", "/media/emmc/live.ctb", 9, 9); history_track(&h);
  hk(&h, "idle", "", -1, -1); history_track(&h);

  char prev[37];
  snprintf(prev, sizeof prev, "%s", g_bridge.task_id);
  snprintf(g_bridge.client_file, sizeof g_bridge.client_file, "/local/live.ctb");
  CHECK(history_begin("n.ctb", "/local/n.ctb") != NULL && ids(l, 8) == 2);
  history_abort();
  CHECK(ids(l, 8) == 1 && strcmp(l[0], id) == 0);
  CHECK(strcmp(g_bridge.task_id, prev) == 0 && strcmp(g_bridge.client_file, "/local/live.ctb") == 0);
  CHECK(history_init(path, 50) == 0 && ids(l, 8) == 1);        // and the file agrees
  history_abort();                                              // nothing to abort
  CHECK(ids(l, 8) == 1);

  g_bridge.task_id[0] = 0;                                      // abort back to "no task"
  history_recover(&h);
  CHECK(history_begin("m.ctb", NULL) != NULL);
  history_abort();
  CHECK(g_bridge.task_id[0] == 0 && ids(l, 8) == 1);
}

static void test_save_failure_keeps_ram_record(void) {
  char l[8][37], bad[300], tmpl[] = "/tmp/test_historyROXXXXXX";
  CHECK(mkdtemp(tmpl));
  snprintf(bad, sizeof bad, "%s/blocker", tmpl);
  FILE *f = fopen(bad, "w");                     // a file where the directory should be: every save fails
  CHECK(f);
  fclose(f);
  snprintf(bad, sizeof bad, "%s/blocker/history.json", tmpl);
  CHECK(history_init(bad, 50) == 0);
  const char *id = history_begin("ro.ctb", NULL);
  CHECK(id && ids(l, 8) == 1);
  history_end(1, 3, 0);
  CHECK(one(l[0]) == 0 && fld("EndTime") > 0 && fld("TaskStatus") == 1);
}

static void test_recover_resumes_same_file(void) {
  char l[8][37], keep[37];
  struct hk_status h;
  unlink(path);
  CHECK(history_init(path, 50) == 0);
  hk(&h, "idle", "", -1, -1);
  history_recover(&h);
  snprintf(keep, sizeof keep, "%s", history_begin("r.ctb", "/local/r.ctb"));
  hk(&h, "printing", "/media/emmc/r.ctb", 1, 9); history_track(&h);     // file adopted and persisted

  g_bridge.task_id[0] = 0;                       // restart while the same file still prints
  CHECK(history_init(path, 50) == 0);
  history_recover(&h);
  CHECK(ids(l, 8) == 1 && strcmp(g_bridge.task_id, keep) == 0);
  CHECK(one(keep) == 0 && fld("EndTime") == 0);
  hk(&h, "complete", "/media/emmc/r.ctb", 9, 9); history_track(&h);     // the resumed task ends normally
  CHECK(one(keep) == 0 && fld("TaskStatus") == 1 && fld("EndTime") > 0);
}

static void test_recover_resumes_by_name_without_path(void) {
  char l[8][37], keep[37];
  struct hk_status h;
  unlink(path);
  CHECK(history_init(path, 50) == 0);
  hk(&h, "idle", "", -1, -1);
  history_recover(&h);
  snprintf(keep, sizeof keep, "%s", history_begin("n.ctb", "/local/n.ctb"));   // crash before the path is known
  g_bridge.task_id[0] = 0;
  CHECK(history_init(path, 50) == 0);
  hk(&h, "printing", "/media/emmc/n.ctb", 1, 9);
  history_recover(&h);
  CHECK(ids(l, 8) == 1 && strcmp(g_bridge.task_id, keep) == 0);
  CHECK(one(keep) == 0 && fld("EndTime") == 0);
}

static void test_history_recover_closes_orphan(void) {
  char l[8][37], keep[37];
  struct hk_status h;
  // open record, printer now prints another file
  unlink(path);
  CHECK(history_init(path, 50) == 0);
  hk(&h, "idle", "", -1, -1);
  history_recover(&h);
  snprintf(keep, sizeof keep, "%s", history_begin("o.ctb", NULL));
  hk(&h, "printing", "/media/emmc/o.ctb", 1, 9); history_track(&h);
  CHECK(history_init(path, 50) == 0);
  hk(&h, "printing", "/media/emmc/other.ctb", 1, 9);
  history_recover(&h);
  CHECK(ids(l, 8) == 2);                         // the orphan plus a task for the print running now
  CHECK(one(keep) == 0 && fld("TaskStatus") == 3 && fld("EndTime") > 0);
  CHECK(strcmp(g_bridge.task_id, keep) != 0 && g_bridge.task_id[0]);

  // open record, printer idle
  unlink(path);
  CHECK(history_init(path, 50) == 0);
  snprintf(keep, sizeof keep, "%s", history_begin("o2.ctb", NULL));
  CHECK(history_init(path, 50) == 0);
  hk(&h, "idle", "", -1, -1);
  history_recover(&h);
  CHECK(ids(l, 8) == 1 && one(keep) == 0 && fld("TaskStatus") == 3 && fld("EndTime") > 0);
}

static void test_corrupt(void) {
  char l[8][37], bad[300];
  snprintf(bad, sizeof bad, "%s.bad", path);
  unlink(bad);
  FILE *f = fopen(path, "w");
  CHECK(f);
  fputs("{\"Tasks\":[{\"TaskId\":", f);          // truncated
  fclose(f);
  CHECK(history_init(path, 50) == 0);
  CHECK(ids(l, 8) == 0);
  CHECK(access(bad, F_OK) == 0 && access(path, F_OK) != 0);
  history_begin("c.ctb", NULL);                  // and it works afterwards
  history_end(1, 1, 0);
  CHECK(history_init(path, 50) == 0 && ids(l, 8) == 1);

  f = fopen(path, "w");                          // valid JSON, wrong shape
  CHECK(f);
  fputs("[1,2,3]", f);
  fclose(f);
  CHECK(history_init(path, 50) == 0 && ids(l, 8) == 0);
}

static void test_long_names(void) {
  char l[8][37], big[700], keep[37], nm[700];
  unlink(path);
  CHECK(history_init(path, 50) == 0);
  memset(big, 'a', sizeof big);
  memcpy(big + 126, "\xc3\xa9", 2);              // e-acute straddling the 127-byte bound
  big[sizeof big - 1] = 0;
  snprintf(keep, sizeof keep, "%s", history_begin(big, big));
  history_end(1, 1, 0);
  CHECK(history_init(path, 50) == 0 && ids(l, 8) == 1);
  CHECK(one(keep) == 0);
  sfld("TaskName", nm, sizeof nm);               // jstr succeeded, so the text is still valid UTF-8
  CHECK(strlen(nm) == 126);
}

int main(void) {
  char tmpl[] = "/tmp/test_historyXXXXXX";
  CHECK(mkdtemp(tmpl));
  snprintf(dir, sizeof dir, "%s/sub/var", tmpl);
  snprintf(path, sizeof path, "%s/history.json", dir);
  test_uuid();
  test_begin_end_reload_cap();
  test_details_ids();
  test_track();
  test_unreachable_hook_ignored();
  test_begin_refused_while_printing_and_abort();
  test_save_failure_keeps_ram_record();
  test_recover_resumes_same_file();
  test_recover_resumes_by_name_without_path();
  test_history_recover_closes_orphan();
  test_corrupt();
  test_long_names();
  return 0;
}
