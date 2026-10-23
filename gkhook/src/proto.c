// Request parsing and reply building for the gkhook control protocol.
#include "proto.h"

#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

// ---- request: a tiny scanner over one flat JSON object ----

static const char* skip_ws(const char* p, const char* e) {
  while (p < e && (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n')) p++;
  return p;
}

static int hexv(char c) {
  if (c >= '0' && c <= '9') return c - '0';
  if (c >= 'a' && c <= 'f') return c - 'a' + 10;
  if (c >= 'A' && c <= 'F') return c - 'A' + 10;
  return -1;
}

// p points at the opening quote. Decodes into dst (cap bytes incl. NUL) unless dst is NULL.
// Returns pointer past the closing quote, or NULL when malformed / too long / embeds NUL.
static const char* read_string(const char* p, const char* e, char* dst, size_t cap) {
  size_t o = 0;
  if (p >= e || *p != '"') return NULL;
  p++;
  while (p < e && *p != '"') {
    unsigned char c = (unsigned char)*p++;
    char out[4];
    size_t ol = 1;
    if (c == '\\') {
      if (p >= e) return NULL;
      c = (unsigned char)*p++;
      switch (c) {
        case '"': case '\\': case '/': out[0] = (char)c; break;
        case 'n': out[0] = '\n'; break;
        case 'r': out[0] = '\r'; break;
        case 't': out[0] = '\t'; break;
        case 'b': out[0] = '\b'; break;
        case 'f': out[0] = '\f'; break;
        case 'u': {
          unsigned v = 0;
          if (e - p < 4) return NULL;
          for (int i = 0; i < 4; i++) {
            int h = hexv(p[i]);
            if (h < 0) return NULL;
            v = v * 16 + (unsigned)h;
          }
          p += 4;
          if (v == 0 || (v >= 0xd800 && v <= 0xdfff)) return NULL;
          if (v < 0x80) { out[0] = (char)v; }
          else if (v < 0x800) { out[0] = (char)(0xc0 | (v >> 6)); out[1] = (char)(0x80 | (v & 0x3f)); ol = 2; }
          else { out[0] = (char)(0xe0 | (v >> 12)); out[1] = (char)(0x80 | ((v >> 6) & 0x3f)); out[2] = (char)(0x80 | (v & 0x3f)); ol = 3; }
          break;
        }
        default: return NULL;
      }
    } else {
      if (c == 0) return NULL;
      out[0] = (char)c;
    }
    if (dst) {
      if (o + ol + 1 > cap) return NULL;
      memcpy(dst + o, out, ol);
    }
    o += ol;
  }
  if (p >= e) return NULL;  // unterminated
  if (dst) dst[o] = 0;
  return p + 1;
}

// Skips a non-string value (number, literal, nested object/array).
static const char* skip_value(const char* p, const char* e) {
  int depth = 0;
  while (p < e) {
    char c = *p;
    if (c == '"') {
      p = read_string(p, e, NULL, 0);
      if (!p) return NULL;
      continue;
    }
    if (c == '{' || c == '[') depth++;
    else if (c == '}' || c == ']') { if (depth == 0) return p; depth--; }
    else if (c == ',' && depth == 0) return p;
    p++;
  }
  return NULL;
}

// Bare JSON integer literal (optional '-', digits, bounded) ending at p's value end. 0 ok.
static int read_int(const char* p, const char* e, long* v) {
  int neg = 0, nd = 0;
  long x = 0;
  if (p < e && *p == '-') { neg = 1; p++; }
  while (p < e && *p >= '0' && *p <= '9') {
    if (++nd > 6) return -1;
    x = x * 10 + (*p - '0');
    p++;
  }
  if (nd == 0) return -1;
  p = skip_ws(p, e);
  if (p >= e || (*p != ',' && *p != '}')) return -1;  // fraction, exponent, junk
  *v = neg ? -x : x;
  return 0;
}

int proto_parse(const char* line, size_t n, struct gk_cmd* out) {
  const char* p = line;
  const char* e = line + n;
  char cmd[16] = "";
  int have_cmd = 0, have_path = 0, have_mm = 0, bad_mm = 0;
  char key[16];

  memset(out, 0, sizeof *out);
  p = skip_ws(p, e);
  if (p >= e || *p != '{') return -1;
  p = skip_ws(p + 1, e);
  if (p < e && *p == '}') return -1;  // {} has no cmd
  for (;;) {
    p = skip_ws(p, e);
    // keys longer than 15 bytes can only be unknown keys
    const char* q = read_string(p, e, key, sizeof key);
    if (!q) {
      q = read_string(p, e, NULL, 0);
      if (!q) return -1;
      key[0] = 0;
    }
    p = skip_ws(q, e);
    if (p >= e || *p != ':') return -1;
    p = skip_ws(p + 1, e);
    if (p < e && *p == '"') {
      if (strcmp(key, "cmd") == 0) {
        q = read_string(p, e, cmd, sizeof cmd);
        if (!q) { cmd[0] = 0; q = read_string(p, e, NULL, 0); if (!q) return -1; strcpy(cmd, "?"); }
        have_cmd = 1;
      } else if (strcmp(key, "path") == 0) {
        q = read_string(p, e, out->path, sizeof out->path);
        if (!q) return -1;
        have_path = out->path[0] != 0;
      } else {
        q = read_string(p, e, NULL, 0);
        if (!q) return -1;
      }
      p = q;
    } else {
      if (strcmp(key, "mm") == 0) {
        long v;
        if (read_int(p, e, &v) != 0) bad_mm = 1;
        else { out->mm = (int)v; have_mm = 1; }
      }
      p = skip_value(p, e);
      if (!p) return -1;
    }
    p = skip_ws(p, e);
    if (p >= e) return -1;
    if (*p == ',') { p++; continue; }
    if (*p == '}') break;
    return -1;
  }
  if (!have_cmd) return -1;
  if (strcmp(cmd, "status") == 0) out->kind = CMD_STATUS;
  else if (strcmp(cmd, "start") == 0) { out->kind = CMD_START; if (!have_path) return -1; }
  else if (strcmp(cmd, "pause") == 0) out->kind = CMD_PAUSE;
  else if (strcmp(cmd, "resume") == 0) out->kind = CMD_RESUME;
  else if (strcmp(cmd, "stop") == 0) out->kind = CMD_STOP;
  else if (strcmp(cmd, "z_move") == 0) {
    out->kind = CMD_Z_MOVE;
    int m = out->mm;
    if (bad_mm || !have_mm || !(m == 1 || m == 10 || m == 100 || m == -1 || m == -10 || m == -100)) return -1;
  }
  else if (strcmp(cmd, "z_top") == 0) out->kind = CMD_Z_TOP;
  else if (strcmp(cmd, "z_home") == 0) out->kind = CMD_Z_HOME;
  else if (strcmp(cmd, "z_stop") == 0) out->kind = CMD_Z_STOP;
  else return -2;
  if (out->kind != CMD_START) out->path[0] = 0;
  if (out->kind != CMD_Z_MOVE) out->mm = 0;
  return 0;
}

// ---- replies ----

struct wr { char* b; size_t cap, o; int bad; };

static void put(struct wr* w, const char* fmt, ...) {
  va_list ap;
  int r;
  if (w->bad) return;
  va_start(ap, fmt);
  r = vsnprintf(w->b + w->o, w->cap - w->o, fmt, ap);
  va_end(ap);
  if (r < 0 || (size_t)r >= w->cap - w->o) w->bad = 1;
  else w->o += (size_t)r;
}

// JSON string body: escape " and \, drop control characters, keep at most maxin input bytes.
static void put_esc(struct wr* w, const char* s, size_t maxin) {
  for (size_t i = 0; s[i] && i < maxin && !w->bad; i++) {
    unsigned char c = (unsigned char)s[i];
    if (c < 0x20 || c == 0x7f) continue;
    if (w->o + 3 > w->cap) { w->bad = 1; return; }
    if (c == '"' || c == '\\') w->b[w->o++] = '\\';
    w->b[w->o++] = (char)c;
  }
}

static size_t finish(struct wr* w) {
  if (w->bad || w->o + 2 > w->cap || w->o + 1 >= PROTO_MAX) return 0;
  w->b[w->o++] = '\n';
  return w->o;
}

size_t proto_reply_ok(char* buf, size_t cap) {
  struct wr w = { buf, cap, 0, 0 };
  put(&w, "{\"ok\":true}");
  return finish(&w);
}

size_t proto_reply_err(char* buf, size_t cap, const char* msg) {
  struct wr w = { buf, cap, 0, 0 };
  put(&w, "{\"ok\":false,\"error\":\"");
  put_esc(&w, msg, 512);
  put(&w, "\"}");
  return finish(&w);
}

static void put_num(struct wr* w, const char* key, long v) {
  if (v < 0) put(w, ",\"%s\":null", key);
  else put(w, ",\"%s\":%ld", key, v);
}

size_t proto_reply_status(char* buf, size_t cap, const struct gk_status* st) {
  struct wr w = { buf, cap, 0, 0 };
  put(&w, "{\"ok\":true,\"state\":\"");
  put_esc(&w, st->state, sizeof st->state);
  put(&w, "\",\"file\":");
  if (st->have_file) {
    put(&w, "\"");
    put_esc(&w, st->file, 400);
    put(&w, "\"");
  } else {
    put(&w, "null");
  }
  put_num(&w, "layer", st->layer);
  put_num(&w, "total_layers", st->total_layers);
  if (!isfinite(st->progress) || st->progress < 0) put(&w, ",\"progress\":null");
  else {  // fixed point: printf %f would follow chitu's locale
    long c = (long)(st->progress * 100.0 + 0.5);
    put(&w, ",\"progress\":%ld.%02ld", c / 100, c % 100);
  }
  put_num(&w, "elapsed_s", st->elapsed_s);
  put_num(&w, "remaining_s", st->remaining_s);
  if (!st->z_avail) put(&w, ",\"z\":null");
  else {
    put(&w, ",\"z\":{\"pos\":");
    if (st->z_homed && isfinite(st->z_pos) && fabs(st->z_pos) < 1e6) {
      long c = (long)(fabs(st->z_pos) * 100.0 + 0.5);  // fixed point: no locale
      put(&w, "%s%ld.%02ld", st->z_pos < 0 && c ? "-" : "", c / 100, c % 100);
    } else put(&w, "null");
    put(&w, ",\"homed\":%s,\"moving\":%s}", st->z_homed ? "true" : "false", st->z_moving ? "true" : "false");
  }
  put(&w, ",\"phase\":");
  if (st->phase[0]) put(&w, "\"%s\"", st->phase);  // fixed names from tick.c: nothing to escape
  else put(&w, "null");
  if (st->screen_x > 0 && st->screen_y > 0) put(&w, ",\"screen\":[%d,%d]", st->screen_x, st->screen_y);
  else put(&w, ",\"screen\":null");
  if (st->message[0]) {
    put(&w, ",\"message\":\"");
    put_esc(&w, st->message, sizeof st->message);
    put(&w, "\"");
  }
  put(&w, "}");
  return finish(&w);
}
