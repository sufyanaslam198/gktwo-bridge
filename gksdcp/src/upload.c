#include "upload.h"
#include "crypto.h"
#include "files.h"
#include "hookw.h"
#include "json.h"
#include "loop.h"
#include "sdcp.h"
#include "status.h"
#include "usbcopy.h"
#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

// ======================= multipart parser =======================

enum { MP_PRE, MP_AFTER, MP_HEAD, MP_BODY, MP_DONE };

int mp_init(struct mp *mp, const char *boundary, const struct mp_cb *cb) {
  size_t n = strlen(boundary);
  if (n == 0 || n > MP_BOUNDARY_MAX || strpbrk(boundary, "\r\n")) return -1;
  memset(mp, 0, sizeof *mp);
  memcpy(mp->d, "\r\n--", 4);
  memcpy(mp->d + 4, boundary, n);
  mp->dl = (int)n + 4;
  mp->m = 2;                      // the body may start with the boundary itself: act as if a CRLF came first
  mp->state = MP_PRE;
  mp->cb = *cb;
  return 0;
}

int mp_done(const struct mp *mp) { return mp->rc == 0 && mp->state == MP_DONE; }

// Body bytes of the current part (dropped before the first boundary).
static void emit(struct mp *mp, const char *p, size_t n) {
  if (mp->state == MP_BODY && n && mp->cb.data(mp->cb.u, p, n)) mp->rc = MP_ABORT;
}

// Value of key="..." in a header line, when key starts a parameter. 1 found / 0 absent / 2 found but too long
// (out holds the truncated text) / -1 unterminated.
static int param(const char *line, size_t len, const char *key, char *out, size_t cap) {
  size_t kl = strlen(key);
  for (size_t k = 1; k + kl + 2 <= len; k++) {
    if (strncasecmp(line + k, key, kl) != 0 || line[k + kl] != '=' || line[k + kl + 1] != '"') continue;
    if (line[k - 1] != ' ' && line[k - 1] != ';' && line[k - 1] != '\t') continue;   // not the tail of filename=
    const char *v = line + k + kl + 2, *e = memchr(v, '"', len - (size_t)(v - line));
    if (!e) return -1;
    size_t n = (size_t)(e - v);
    if (n >= cap) { memcpy(out, v, cap - 1); out[cap - 1] = 0; return 2; }   // too long: truncated, never a valid name
    memcpy(out, v, n);
    out[n] = 0;
    return 1;
  }
  return 0;
}

// The header block is complete: find name/filename, enforce the part order, call part().
static void head_done(struct mp *mp) {
  const char *s = mp->hb, *end = mp->hb + mp->hn;
  int have_name = 0, have_fname = 0;
  mp->name[0] = mp->fname[0] = 0;
  while (s < end) {
    const char *e = memchr(s, '\r', (size_t)(end - s));
    if (!e) e = end;
    size_t len = (size_t)(e - s);
    if (len > 20 && strncasecmp(s, "content-disposition:", 20) == 0) {
      int a = param(s + 20, len - 20, "name", mp->name, sizeof mp->name);
      int b = param(s + 20, len - 20, "filename", mp->fname, sizeof mp->fname);
      if (a < 0 || b < 0 || a == 2) { mp->rc = MP_ERR; return; }    // a long filename is left to the caller to refuse
      have_name = a;
      have_fname = b != 0;
    }
    s = end - e >= 2 ? e + 2 : end;
  }
  int is_file = have_fname || (have_name && strcmp(mp->name, "File") == 0);
  if (!have_name || mp->file_seen) { mp->rc = MP_ERR; return; }     // a part after the file part is refused
  if (is_file) mp->file_seen = 1;
  if (mp->cb.part(mp->cb.u, mp->name, is_file ? mp->fname : NULL)) { mp->rc = MP_ABORT; return; }
  mp->state = MP_BODY;
  mp->m = 0;
}

// The delimiter was matched completely.
static void delimiter(struct mp *mp) {
  if (mp->state == MP_BODY && mp->cb.end(mp->cb.u)) { mp->rc = MP_ABORT; return; }
  mp->state = MP_AFTER;
  mp->after_n = 0;
}

// Scan for the delimiter in a preamble or part body; returns the bytes consumed.
static size_t scan(struct mp *mp, const char *p, size_t n) {
  size_t i = 0, run = 0;                    // [run, i) is data not yet emitted, valid while m == 0
  while (i < n && !mp->rc) {
    if (mp->m == 0) {
      const char *q = memchr(p + i, '\r', n - i);
      if (!q) { i = n; break; }
      i = (size_t)(q - p);
      emit(mp, p + run, i - run);
      mp->m = 1;
      i++;
      continue;
    }
    if (p[i] == mp->d[mp->m]) {
      i++;
      if (++mp->m == mp->dl) { mp->m = 0; delimiter(mp); return i; }
      continue;
    }
    emit(mp, mp->d, (size_t)mp->m);         // the partial match was data after all; CR cannot start inside it
    mp->m = 0;
    run = i;                                // retry this byte from scratch
  }
  if (mp->m == 0 && !mp->rc) emit(mp, p + run, i - run);
  return i;
}

int mp_feed(struct mp *mp, const char *p, size_t n) {
  size_t i = 0;
  while (i < n && !mp->rc && mp->state != MP_DONE) {
    switch (mp->state) {
      case MP_PRE:
      case MP_BODY:
        i += scan(mp, p + i, n - i);
        break;
      case MP_AFTER:
        mp->after[mp->after_n++] = p[i++];
        if (mp->after_n == 2) {
          if (memcmp(mp->after, "--", 2) == 0) mp->state = MP_DONE;
          else if (memcmp(mp->after, "\r\n", 2) == 0) { mp->state = MP_HEAD; mp->hn = 0; }
          else mp->rc = MP_ERR;
        }
        break;
      case MP_HEAD:
        if (mp->hn == MP_HEAD_MAX) { mp->rc = MP_ERR; break; }
        mp->hb[mp->hn++] = p[i++];
        if (mp->hn >= 4 && memcmp(mp->hb + mp->hn - 4, "\r\n\r\n", 4) == 0) head_done(mp);
        break;
    }
  }
  return mp->rc;
}

// ======================= upload session =======================

#define MAX_REQ  4
#define VAL_MAX  72
#define NAME_BUF 201                // name_valid allows 200 bytes
#define PATH_BYTES 1100
#define UPLOAD_TAKEOVER_MS 5000       // an idle session this old gives way to a different Uuid (DragonFruit never cancels)
#define MAX_TOTAL (64LL << 30)        // largest TotalSize accepted

enum { F_MD5, F_CHECK, F_OFFSET, F_UUID, F_TOTAL, F_N };
static const char *const field_names[F_N] = { "S-File-MD5", "Check", "Offset", "Uuid", "TotalSize" };

// One POST being parsed. The session below outlives it.
struct req {
  int used;
  uint32_t conn;
  struct mp mp;
  int cur;                          // field being collected, -1 when skipping
  size_t flen;
  char fv[F_N][VAL_MAX];
  int have[F_N];
  int writing;                      // this request owns the session's current chunk
  int file_done;
  int aborted;                      // Cmd 255 cancelled the session under it
  struct md5 saved;                 // md5 and size at the start of the chunk, for a rollback
  long long chunk_start;
  int err_code;                     // set when a callback refuses
  const char *err_field, *err_msg;
};

static struct {
  int active;
  char uuid[VAL_MAX];
  char name[NAME_BUF];
  char tmp[PATH_BYTES], fin[PATH_BYTES];
  int fd;
  long long written, total;
  struct md5 md5;
  long long last_ms;
  uint32_t streaming;               // connection id of the request writing now, 0 none
} S = { .fd = -1 };

static struct { char dir[PATH_BYTES]; long long stale_ms; int allow_tmpfs; } cfg;
static struct req reqs[MAX_REQ];
static int g_cleaned;               // the leftover .part files on the mounted storage are gone

static long long mono_ms(void) {
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return (long long)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

static struct req *req_find(uint32_t conn) {
  for (int i = 0; i < MAX_REQ; i++)
    if (reqs[i].used && reqs[i].conn == conn) return &reqs[i];
  return NULL;
}

// ---- replies ----

// DragonFruit looks only at the HTTP status, so every refusal is non-2xx; the body stays the spec failure JSON.
static int failure_status(int code, const char *msg) {
  if (msg && (!strcmp(msg, "printer busy") || !strcmp(msg, "printer state unknown") ||
              !strcmp(msg, "another upload is running") || !strcmp(msg, "upload cancelled"))) return 409;
  if (msg && !strcmp(msg, "not enough space")) return 507;
  if (msg && !strcmp(msg, "storage not mounted")) return 503;
  if (code == -3 || (msg && !strcmp(msg, "md5 mismatch"))) return 500;
  return 400;
}

static void reply_failure(struct conn *c, int code, const char *field, const char *msg) {
  char buf[512];
  struct jw w;
  jw_init(&w, buf, sizeof buf);
  jw_raw(&w, "{\"code\":\"111111\",\"messages\":[{\"field\":\"common_field\",\"message\":");
  jw_int(&w, code);
  jw_raw(&w, "}");
  if (msg) {
    jw_raw(&w, ",{\"field\":"); jw_str(&w, field);
    jw_raw(&w, ",\"message\":"); jw_str(&w, msg);
    jw_raw(&w, "}");
  }
  jw_raw(&w, "],\"data\":null,\"success\":false}");
  conn_http_reply(c, failure_status(code, msg), "application/json", buf, w.len);
}

static void reply_success(struct conn *c) {
  static const char ok[] = "{\"code\":\"000000\",\"messages\":null,\"data\":{},\"success\":true}";
  conn_http_reply(c, 200, "application/json", ok, sizeof ok - 1);
}

static void req_refuse(struct req *r, int code, const char *field, const char *msg) {
  r->err_code = code;
  r->err_field = field;
  r->err_msg = msg;
}

// ---- session ----

static void session_end(int unlink_tmp) {
  if (S.fd >= 0) close(S.fd);
  S.fd = -1;
  if (unlink_tmp && S.tmp[0]) unlink(S.tmp);
  S.active = 0;
  S.streaming = 0;
  g_bridge.uploading = 0;
  sdcp_status_dirty();
}

// Undo the chunk r was writing: back to where it started, md5 included. A session with nothing
// written yet is dropped when drop_empty.
static void chunk_abort(struct req *r, int drop_empty) {
  if (!r->writing) return;
  r->writing = 0;
  if (!S.active || S.streaming != r->conn) return;
  if (S.fd >= 0 && ftruncate(S.fd, r->chunk_start) < 0) { session_end(1); return; }
  S.written = r->chunk_start;
  S.md5 = r->saved;
  S.streaming = 0;
  S.last_ms = mono_ms();
  if (drop_empty && r->chunk_start == 0) session_end(1);
}

// A cancelled upload (DragonFruit never sends Cmd 255): no chunk in flight and none for a while.
static int abandoned(void) {
  return S.active && !S.streaming && mono_ms() - S.last_ms > UPLOAD_TAKEOVER_MS;
}

// NULL when an existing file may be replaced now, else the refusal message.
static const char *replace_refusal(void) {
  struct hk_status h;
  hookw_status(&h);
  if (!h.hook_ok) return "printer state unknown";       // the touchscreen can print that file
  int ok = !strcmp(h.state, "idle") || !strcmp(h.state, "complete") || !strcmp(h.state, "error");
  return ok && !cmd_print_start_pending() ? NULL : "printer busy";
}

static int hex32(const char *s) {
  if (strlen(s) != 32) return 0;
  for (int i = 0; i < 32; i++)
    if (!isxdigit((unsigned char)s[i])) return 0;
  return 1;
}

static int parse_ll(const char *s, long long *out) {
  char *end;
  errno = 0;
  if (!*s) return -1;
  long long v = strtoll(s, &end, 10);
  if (*end || errno) return -1;
  *out = v;
  return 0;
}

static int has_ctb(const char *name) {
  size_t n = strlen(name);
  return n >= 4 && strcasecmp(name + n - 4, ".ctb") == 0;
}

static int uuid_ok(const char *u) {
  size_t n = strlen(u);
  if (n == 0 || n > 64) return 0;
  for (size_t i = 0; i < n; i++)
    if (!isalnum((unsigned char)u[i]) && u[i] != '-' && u[i] != '_') return 0;
  return 1;
}

// The File part's headers arrived: validate everything and open (or continue) the session.
// 0 ok (the request now owns the chunk) / -1 refused (req_refuse was called).
static int begin_file(struct req *r, const char *filename) {
  if (!r->have[F_OFFSET] || !r->have[F_UUID] || !r->have[F_TOTAL]) {
    req_refuse(r, -4, "common_field", "missing field before File");
    return -1;
  }
  long long offset, total;
  if (parse_ll(r->fv[F_OFFSET], &offset) < 0) { req_refuse(r, -4, "common_field", "invalid Offset"); return -1; }
  if (offset < 0) { req_refuse(r, -1, NULL, NULL); return -1; }
  if (parse_ll(r->fv[F_TOTAL], &total) < 0 || total <= 0 || total > MAX_TOTAL) { req_refuse(r, -4, "common_field", "invalid TotalSize"); return -1; }
  if (!name_valid(filename) || !has_ctb(filename)) { req_refuse(r, -4, "filename", "invalid file name"); return -1; }
  if (!uuid_ok(r->fv[F_UUID])) { req_refuse(r, -4, "common_field", "invalid Uuid"); return -1; }

  struct hk_status h;
  hookw_status(&h);
  if (!strcmp(h.state, "printing") || !strcmp(h.state, "paused")) { req_refuse(r, -4, "common_field", "printer busy"); return -1; }
  if (usbcopy_active(NULL, NULL) || g_bridge.usb_copying) { req_refuse(r, -4, "common_field", "printer busy"); return -1; }
  if (!files_storage_ok(cfg.dir, cfg.allow_tmpfs)) { req_refuse(r, -4, "common_field", "storage not mounted"); return -1; }

  // an abandoned session gives way to another Uuid, once this request has passed every check below
  int takeover = abandoned() && strcmp(S.uuid, r->fv[F_UUID]) != 0;
  int cont = S.active && !takeover;                 // the chunk belongs to the open session
  if (cont) {
    if (strcmp(S.uuid, r->fv[F_UUID]) != 0) { req_refuse(r, -4, "common_field", "another upload is running"); return -1; }
    if (strcmp(S.name, filename) != 0) { req_refuse(r, -4, "filename", "file name changed"); return -1; }
    if (S.total != total) { req_refuse(r, -4, "common_field", "total size changed"); return -1; }
    if (S.streaming) {
      // a client that resends the chunk its old connection was still sending (half-open socket): take over
      struct req *o = req_find(S.streaming);
      if (!o || offset != o->chunk_start) { req_refuse(r, -4, "common_field", "another upload is running"); return -1; }
      chunk_abort(o, 0);
      o->aborted = 1;
    }
    if (offset != S.written) { req_refuse(r, -2, NULL, NULL); return -1; }
  } else if (offset != 0) {
    req_refuse(r, -2, NULL, NULL);
    return -1;
  }

  char fin[PATH_BYTES];
  if (snprintf(fin, sizeof fin, "%s/%s", cfg.dir, filename) >= (int)sizeof fin) { req_refuse(r, -3, NULL, NULL); return -1; }
  struct stat st;
  const char *why;
  if (lstat(fin, &st) == 0 && (why = replace_refusal()) != NULL) { req_refuse(r, -4, "common_field", why); return -1; }
  long long done = cont ? S.written : 0, free_b = files_free_bytes(cfg.dir);
  if (takeover && free_b >= 0) free_b += S.written;  // the old partial file goes
  if (free_b < files_reserve() || total - done > free_b - files_reserve()) { req_refuse(r, -4, "common_field", "not enough space"); return -1; }

  if (takeover) session_end(1);
  if (!S.active) {
    char u8[9];
    int k = 0;
    for (const char *u = r->fv[F_UUID]; *u && k < 8; u++)
      if (isalnum((unsigned char)*u)) u8[k++] = *u;
    if (!k) u8[k++] = '0';
    u8[k] = 0;
    if (snprintf(S.tmp, sizeof S.tmp, "%s/.%s.%s.part", cfg.dir, filename, u8) >= (int)sizeof S.tmp) {
      S.tmp[0] = 0;
      req_refuse(r, -3, NULL, NULL);
      return -1;
    }
    S.fd = open(S.tmp, O_WRONLY | O_CREAT | O_TRUNC | O_NOFOLLOW | O_CLOEXEC, 0644);
    if (S.fd < 0) { S.tmp[0] = 0; req_refuse(r, -3, NULL, NULL); return -1; }
    if (flock(S.fd, LOCK_EX | LOCK_NB) < 0) {          // a stale-file cleanup must not unlink a live temp file
      close(S.fd);
      S.fd = -1;
      unlink(S.tmp);
      S.tmp[0] = 0;
      req_refuse(r, -3, NULL, NULL);
      return -1;
    }
    snprintf(S.fin, sizeof S.fin, "%s", fin);
    snprintf(S.name, sizeof S.name, "%s", filename);
    snprintf(S.uuid, sizeof S.uuid, "%s", r->fv[F_UUID]);
    S.total = total;
    S.written = 0;
    md5_init(&S.md5);
    S.active = 1;
    g_bridge.uploading = 1;
    sdcp_status_dirty();
  }
  r->saved = S.md5;
  r->chunk_start = S.written;
  r->writing = 1;
  S.streaming = r->conn;
  S.last_ms = mono_ms();
  return 0;
}

// File bytes of the chunk.
static int file_write(struct req *r, const char *p, size_t n) {
  if (!S.active || S.streaming != r->conn) { req_refuse(r, -4, "common_field", "upload cancelled"); return -1; }
  if ((long long)n > S.total - S.written) { req_refuse(r, -4, "common_field", "more data than TotalSize"); return -1; }
  size_t off = 0;
  while (off < n) {
    ssize_t w = pwrite(S.fd, p + off, n - off, (off_t)(S.written + (long long)off));
    if (w < 0 && errno == EINTR) continue;
    if (w <= 0) { req_refuse(r, -3, NULL, NULL); return -1; }
    off += (size_t)w;
  }
  md5_update(&S.md5, p, n);
  S.written += (long long)n;
  S.last_ms = mono_ms();
  return 0;
}

static int cb_part(void *u, const char *name, const char *filename) {
  struct req *r = u;
  if (filename) return begin_file(r, filename);
  r->cur = -1;
  r->flen = 0;
  for (int i = 0; i < F_N; i++)
    if (strcmp(name, field_names[i]) == 0) r->cur = i;
  return 0;
}

static int cb_data(void *u, const char *p, size_t n) {
  struct req *r = u;
  if (r->writing) return file_write(r, p, n);
  if (r->cur < 0) return 0;
  if (r->flen + n >= VAL_MAX) { req_refuse(r, -4, "common_field", "field too long"); return -1; }
  memcpy(r->fv[r->cur] + r->flen, p, n);
  r->flen += n;
  return 0;
}

static int cb_end(void *u) {
  struct req *r = u;
  if (r->writing) { r->file_done = 1; return 0; }
  if (r->cur >= 0) {
    r->fv[r->cur][r->flen] = 0;
    r->have[r->cur] = 1;
    r->cur = -1;
  }
  return 0;
}

// The last byte of the file is in: verify, flush, move into place. 0 ok / -1 refused (session gone).
static int finalize(struct req *r) {
  if (!strcmp(r->fv[F_CHECK], "1") && hex32(r->fv[F_MD5])) {
    char hex[33];
    struct md5 m = S.md5;
    md5_hex(&m, hex);
    if (strcasecmp(hex, r->fv[F_MD5]) != 0) {
      session_end(1);
      sdcp_error(1);
      req_refuse(r, -4, "common_field", "md5 mismatch");
      return -1;
    }
  }
  struct stat st;
  const char *why;
  if (lstat(S.fin, &st) == 0 && (why = replace_refusal()) != NULL) {   // a print began during the upload
    session_end(1);
    req_refuse(r, -4, "common_field", why);
    return -1;
  }
  int fd = S.fd;
  S.fd = -1;
  int ok = fsync(fd) == 0;
  ok = close(fd) == 0 && ok;
  if (!ok || rename(S.tmp, S.fin) < 0) {
    session_end(1);
    req_refuse(r, -3, NULL, NULL);
    return -1;
  }
  session_end(0);
  return 0;
}

static void req_free(struct req *r) { r->used = 0; }

// Answer a refused request and drop it. Returns 1 (abort the body).
static int req_fail(struct req *r, struct conn *c) {
  if (!r->err_code) req_refuse(r, -4, "common_field", "malformed upload request");
  chunk_abort(r, 1);
  reply_failure(c, r->err_code, r->err_field, r->err_msg);
  req_free(r);
  return 1;
}

// ---- loop hooks ----

// The boundary parameter of a multipart/form-data Content-Type, or NULL.
static int boundary_of(const char *ctype, char *out, size_t cap) {
  if (strncasecmp(ctype, "multipart/form-data", 19) != 0) return -1;
  const char *p = strcasestr(ctype, "boundary=");
  if (!p) return -1;
  p += 9;
  size_t n = 0;
  if (*p == '"') {
    p++;
    while (p[n] && p[n] != '"') n++;
  } else {
    while (p[n] && p[n] != ';' && p[n] != ' ') n++;
  }
  if (n == 0 || n >= cap) return -1;
  memcpy(out, p, n);
  out[n] = 0;
  return 0;
}

static int up_head(struct conn *c, const struct http_head *h) {
  if (strcmp(h->method, "POST") != 0) return 0;
  char boundary[MP_BOUNDARY_MAX + 2];
  struct req *r = NULL;
  for (int i = 0; i < MAX_REQ && !r; i++)
    if (!reqs[i].used) r = &reqs[i];
  if (h->has_te) { reply_failure(c, -4, "common_field", "Transfer-Encoding is not supported"); return 0; }
  if (boundary_of(h->ctype, boundary, sizeof boundary) < 0) { reply_failure(c, -4, "common_field", "multipart/form-data required"); return 0; }
  if (!r) { reply_failure(c, -4, "common_field", "another upload is running"); return 0; }
  memset(r, 0, sizeof *r);
  struct mp_cb cb = { cb_part, cb_data, cb_end, r };
  if (mp_init(&r->mp, boundary, &cb) < 0) { reply_failure(c, -4, "common_field", "invalid boundary"); return 0; }
  r->used = 1;
  r->conn = conn_id(c);
  r->cur = -1;
  return 1;
}

static int up_data(struct conn *c, const char *buf, size_t n) {
  struct req *r = req_find(conn_id(c));
  if (!r) { reply_failure(c, -4, "common_field", "malformed upload request"); return 1; }
  if (r->aborted) { req_refuse(r, -4, "common_field", "upload cancelled"); return req_fail(r, c); }
  if (buf) {
    if (mp_feed(&r->mp, buf, n) != 0) return req_fail(r, c);
    return 0;
  }
  // end of the body
  if (!mp_done(&r->mp) || !r->file_done) {
    req_refuse(r, -4, "common_field", r->file_done ? "incomplete multipart request" : "File part missing");
    return req_fail(r, c);
  }
  r->writing = 0;
  S.streaming = 0;
  S.last_ms = mono_ms();
  if (S.written == S.total && finalize(r) < 0) return req_fail(r, c);
  reply_success(c);
  req_free(r);
  return 0;
}

static void up_closed(struct conn *c) {
  struct req *r = req_find(conn_id(c));
  if (!r) return;
  chunk_abort(r, 0);             // keep the session: the client may resend this chunk at the same offset
  req_free(r);
}

// ---- public ----

int upload_busy(void) {
  if (abandoned()) session_end(1);
  return S.active;
}

int upload_storage_ok(void) { return files_storage_ok(cfg.dir, cfg.allow_tmpfs); }

int upload_cancel(const char *uuid, const char *filename) {
  if (!S.active) return 1;
  if (!uuid || strcmp(uuid, S.uuid) != 0) return 3;
  if (filename && filename[0] && strcmp(filename, S.name) != 0) return 3;
  for (int i = 0; i < MAX_REQ; i++)
    if (reqs[i].used && reqs[i].writing) { reqs[i].aborted = 1; reqs[i].writing = 0; }
  session_end(1);
  return 0;
}

static int cleanup_stale(const char *dir);

void upload_tick(void) {
  if (S.active && !S.streaming && mono_ms() - S.last_ms > cfg.stale_ms) session_end(1);
  // /media/emmc mounts after gksdcp starts: the leftovers are there to remove only then
  if (!g_cleaned && upload_storage_ok()) g_cleaned = cleanup_stale(cfg.dir) == 0;
}

static void cmd_cancel(struct sdcp_req *r) {
  char uuid[VAL_MAX], name[NAME_BUF + 1];
  int ack, t;
  const char *u = NULL, *f = NULL;
  if (r->data >= 0) {
    t = jget(r->d, r->data, "Uuid");
    if (t >= 0 && jstr(r->d, t, uuid, sizeof uuid) == 0) u = uuid;
    t = jget(r->d, r->data, "FileName");
    if (t >= 0) f = jstr(r->d, t, name, sizeof name) == 0 ? name : "\x01";   // unusable name never matches
  }
  ack = upload_cancel(u, f);
  sdcp_reply(r->conn, r->cmd, r->rid, ack, "");
}

// Leftover temp files from a run that died: .*.part, unless another process holds them. 0 done / -1 no dir.
static int cleanup_stale(const char *dir) {
  DIR *d = opendir(dir);
  if (!d) return -1;
  struct dirent *de;
  while ((de = readdir(d)) != NULL) {
    size_t n = strlen(de->d_name);
    char p[PATH_BYTES + 300];
    if (de->d_name[0] != '.' || n < 6 || strcmp(de->d_name + n - 5, ".part") != 0) continue;
    snprintf(p, sizeof p, "%s/%s", dir, de->d_name);
    int fd = open(p, O_WRONLY | O_NOFOLLOW | O_CLOEXEC);
    if (fd < 0) continue;
    if (flock(fd, LOCK_EX | LOCK_NB) == 0) unlink(p);
    close(fd);
  }
  closedir(d);
  return 0;
}

void upload_init(const char *files_dir, int stale_seconds, int allow_tmpfs) {
  snprintf(cfg.dir, sizeof cfg.dir, "%s", files_dir);
  cfg.stale_ms = (long long)stale_seconds * 1000;
  cfg.allow_tmpfs = allow_tmpfs;
  g_cleaned = 0;
  upload_tick();
  on_upload_head = up_head;
  on_upload_data = up_data;
  on_conn_close = up_closed;
  sdcp_register(255, cmd_cancel);
  loop_every(1000, upload_tick);
}
