#include "upload.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CHECK(c) do { if (!(c)) { fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #c); exit(1); } } while (0)

// Records "<name>[|<filename>]=<data>;" per part.
struct rec { char out[8192]; size_t n; int abort_on; int parts; };

static void put(struct rec *r, const char *s, size_t n) {
  CHECK(r->n + n < sizeof r->out);
  memcpy(r->out + r->n, s, n);
  r->n += n;
  r->out[r->n] = 0;
}

static int cb_part(void *u, const char *name, const char *filename) {
  struct rec *r = u;
  put(r, name, strlen(name));
  if (filename) { put(r, "|", 1); put(r, filename, strlen(filename)); }
  put(r, "=", 1);
  return ++r->parts == r->abort_on;
}
static int cb_data(void *u, const char *p, size_t n) { put(u, p, n); return 0; }
static int cb_end(void *u) { put(u, ";", 1); return 0; }

// Feed msg in pieces of at most step bytes (0 = whole). Returns the first non-zero mp_feed result.
static int run(struct rec *r, const char *boundary, const char *msg, size_t len, size_t step, struct mp *mp) {
  struct mp_cb cb = { cb_part, cb_data, cb_end, r };
  memset(r, 0, sizeof *r);
  CHECK(mp_init(mp, boundary, &cb) == 0);
  if (!step) step = len;
  for (size_t i = 0; i < len; i += step) {
    size_t n = len - i < step ? len - i : step;
    int rc = mp_feed(mp, msg + i, n);
    if (rc) return rc;
  }
  return 0;
}

static void test_basic(void) {
  const char *m =
    "--XyZ\r\nContent-Disposition: form-data; name=\"Offset\"\r\n\r\n0\r\n"
    "--XyZ\r\nContent-Disposition: form-data; name=\"Uuid\"\r\n\r\nabc-def\r\n"
    "--XyZ\r\nContent-Disposition: form-data; name=\"File\"; filename=\"a (2).ctb\"\r\n"
    "Content-Type: application/octet-stream\r\n\r\nDA\r\n-TA\r\n--Xy\r\n--XyX--XyZ\r\n--XyZ--\r\n";
  const char *want = "Offset=0;Uuid=abc-def;File|a (2).ctb=DA\r\n-TA\r\n--Xy\r\n--XyX--XyZ;";
  size_t len = strlen(m);
  struct rec r; struct mp mp;
  for (size_t step = 0; step <= len; step++) {       // whole, then every piece size down to one byte
    CHECK(run(&r, "XyZ", m, len, step, &mp) == 0);
    CHECK(strcmp(r.out, want) == 0);
    CHECK(mp_done(&mp));
  }
}

// every two-way split, so the boundary is cut at every possible place
static void test_every_split(void) {
  const char *m =
    "junk before\r\n--b0und\r\nContent-Disposition: form-data; name=\"Check\"\r\n\r\n1\r\n"
    "--b0und\r\nContent-Disposition: form-data; name=\"File\"; filename=\"x.ctb\"\r\n\r\n"
    "\r\n--b0un\r\n--b0unx\r\r\n--\r\n-\r\r\n--b0und--\r\ntrailer";
  const char *want = "Check=1;File|x.ctb=\r\n--b0un\r\n--b0unx\r\r\n--\r\n-\r;";
  size_t len = strlen(m);
  struct rec r; struct mp mp;
  for (size_t cut = 0; cut <= len; cut++) {
    struct mp_cb cb = { cb_part, cb_data, cb_end, &r };
    memset(&r, 0, sizeof r);
    CHECK(mp_init(&mp, "b0und", &cb) == 0);
    CHECK(mp_feed(&mp, m, cut) == 0);
    CHECK(mp_feed(&mp, m + cut, len - cut) == 0);
    CHECK(strcmp(r.out, want) == 0);
    CHECK(mp_done(&mp));
  }
}

static void test_order_and_duplicates(void) {
  struct rec r; struct mp mp;
  const char *after =
    "--B\r\nContent-Disposition: form-data; name=\"File\"; filename=\"a.ctb\"\r\n\r\nD\r\n"
    "--B\r\nContent-Disposition: form-data; name=\"Offset\"\r\n\r\n0\r\n--B--\r\n";
  CHECK(run(&r, "B", after, strlen(after), 0, &mp) == MP_ERR);       // field after File
  CHECK(strcmp(r.out, "File|a.ctb=D;") == 0);                        // the File part itself was delivered
  const char *twice =
    "--B\r\nContent-Disposition: form-data; name=\"File\"; filename=\"a.ctb\"\r\n\r\nD\r\n"
    "--B\r\nContent-Disposition: form-data; name=\"File\"; filename=\"b.ctb\"\r\n\r\nD\r\n--B--\r\n";
  CHECK(run(&r, "B", twice, strlen(twice), 0, &mp) == MP_ERR);
}

static void test_malformed(void) {
  struct rec r; struct mp mp;
  CHECK(run(&r, "B", "--Bxx\r\n", 7, 0, &mp) == MP_ERR);              // junk after the delimiter
  const char *nohdr = "--B\r\nX-Other: 1\r\n\r\nD\r\n--B--\r\n";
  CHECK(run(&r, "B", nohdr, strlen(nohdr), 0, &mp) == MP_ERR);        // no Content-Disposition name
  const char *trunc = "--B\r\nContent-Disposition: form-data; name=\"File\"; filename=\"a.ctb\"\r\n\r\nDDD";
  CHECK(run(&r, "B", trunc, strlen(trunc), 0, &mp) == 0);
  CHECK(!mp_done(&mp));                                               // never finished
  char big[2048];
  memset(big, 'h', sizeof big);
  memcpy(big, "--B\r\n", 5);
  CHECK(run(&r, "B", big, sizeof big, 0, &mp) == MP_ERR);             // header block too long
  struct mp_cb cb = { cb_part, cb_data, cb_end, &r };
  CHECK(mp_init(&mp, "", &cb) < 0);
  CHECK(mp_init(&mp, "a\r\nb", &cb) < 0);
  char b71[80];
  memset(b71, 'q', 71); b71[71] = 0;
  CHECK(mp_init(&mp, b71, &cb) < 0);
  b71[70] = 0;
  CHECK(mp_init(&mp, b71, &cb) == 0);
}

static void test_abort(void) {
  const char *m =
    "--B\r\nContent-Disposition: form-data; name=\"Offset\"\r\n\r\n0\r\n"
    "--B\r\nContent-Disposition: form-data; name=\"File\"; filename=\"a.ctb\"\r\n\r\nD\r\n--B--\r\n";
  struct rec r; struct mp mp;
  struct mp_cb cb = { cb_part, cb_data, cb_end, &r };
  memset(&r, 0, sizeof r);
  r.abort_on = 2;
  CHECK(mp_init(&mp, "B", &cb) == 0);
  CHECK(mp_feed(&mp, m, strlen(m)) == MP_ABORT);
  CHECK(mp_feed(&mp, m, 1) == MP_ABORT);                              // stays failed
}

static void test_filename_param(void) {
  // "name=" must not match inside filename=; parameter order does not matter
  const char *m =
    "--B\r\ncontent-disposition: form-data; filename=\"q.ctb\"; name=\"File\"\r\n\r\nD\r\n--B--\r\n";
  struct rec r; struct mp mp;
  CHECK(run(&r, "B", m, strlen(m), 0, &mp) == 0);
  CHECK(strcmp(r.out, "File|q.ctb=D;") == 0);
  const char *u = "--B\r\nContent-Disposition: form-data; name=\"File\"; filename=\"Drache_\xc3\x9c (2).ctb\"\r\n\r\nD\r\n--B--\r\n";
  CHECK(run(&r, "B", u, strlen(u), 3, &mp) == 0);
  CHECK(strcmp(r.out, "File|Drache_\xc3\x9c (2).ctb=D;") == 0);
}

int main(void) {
  test_basic();
  test_every_split();
  test_order_and_duplicates();
  test_malformed();
  test_abort();
  test_filename_param();
  return 0;
}
