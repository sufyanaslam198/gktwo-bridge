#include "http.h"
#include <stdlib.h>
#include <string.h>
#include <strings.h>

// Copy [s, s+n) into out[cap]; -1 if it does not fit.
static int copy_field(char *out, size_t cap, const char *s, size_t n) {
  if (n >= cap) return -1;
  memcpy(out, s, n);
  out[n] = 0;
  return 0;
}

static int has_token(const char *v, size_t n, const char *tok) {
  size_t tl = strlen(tok);
  for (size_t i = 0; i + tl <= n; i++)
    if (strncasecmp(v + i, tok, tl) == 0) return 1;
  return 0;
}

static int parse_request_line(const char *s, size_t n, struct http_head *h) {
  const char *sp1 = memchr(s, ' ', n);
  if (!sp1) return -1;
  const char *sp2 = memchr(sp1 + 1, ' ', (size_t)(s + n - sp1 - 1));
  if (!sp2) return -1;
  size_t vn = (size_t)(s + n - sp2 - 1);
  if (vn != 8 || strncmp(sp2 + 1, "HTTP/1.", 7) != 0) return -1;
  if (sp1 == s || sp2 == sp1 + 1) return -1;
  if (copy_field(h->method, sizeof h->method, s, (size_t)(sp1 - s)) < 0) return -1;
  if (copy_field(h->path, sizeof h->path, sp1 + 1, (size_t)(sp2 - sp1 - 1)) < 0) return -1;
  return 0;
}

static int parse_header(const char *s, size_t n, struct http_head *h) {
  const char *colon = memchr(s, ':', n);
  if (!colon || colon == s) return -1;
  size_t kn = (size_t)(colon - s);
  const char *v = colon + 1;
  size_t vn = (size_t)(s + n - v);
  while (vn && (*v == ' ' || *v == '\t')) { v++; vn--; }
  while (vn && (v[vn - 1] == ' ' || v[vn - 1] == '\t')) vn--;

  if (kn == 4 && strncasecmp(s, "host", 4) == 0) return copy_field(h->host, sizeof h->host, v, vn);
  if (kn == 6 && strncasecmp(s, "origin", 6) == 0) {
    h->has_origin = 1;
    return copy_field(h->origin, sizeof h->origin, v, vn);
  }
  if (kn == 12 && strncasecmp(s, "content-type", 12) == 0) return copy_field(h->ctype, sizeof h->ctype, v, vn);
  if (kn == 17 && strncasecmp(s, "sec-websocket-key", 17) == 0) return copy_field(h->ws_key, sizeof h->ws_key, v, vn);
  if (kn == 7 && strncasecmp(s, "upgrade", 7) == 0) {
    if (has_token(v, vn, "websocket")) h->upgrade_ws = 1;
    return 0;
  }
  if (kn == 17 && strncasecmp(s, "transfer-encoding", 17) == 0) { h->has_te = 1; return 0; }
  if (kn == 14 && strncasecmp(s, "content-length", 14) == 0) {
    if (vn == 0 || vn > 15) return -1;
    long long x = 0;
    for (size_t i = 0; i < vn; i++) {
      if (v[i] < '0' || v[i] > '9') return -1;
      x = x * 10 + (v[i] - '0');
    }
    h->clen = x;
  }
  return 0;
}

int http_parse_head(const char *buf, size_t n, struct http_head *h) {
  size_t lim = n < HTTP_MAX_HEAD ? n : HTTP_MAX_HEAD;
  size_t end = 0;
  for (size_t i = 0; i + 4 <= lim; i++)
    if (buf[i] == '\r' && buf[i + 1] == '\n' && buf[i + 2] == '\r' && buf[i + 3] == '\n') { end = i + 4; break; }
  if (!end) return n >= HTTP_MAX_HEAD ? -1 : 0;

  memset(h, 0, sizeof *h);
  if (memchr(buf, 0, end)) return -1;

  size_t pos = 0;
  int first = 1;
  while (pos < end - 2) {            // the last CRLF of the terminator is not a line
    const char *ln = buf + pos;
    const char *cr = memchr(ln, '\r', end - pos);
    if (!cr || cr[1] != '\n') return -1;
    size_t len = (size_t)(cr - ln);
    if (len == 0) break;
    if (first) {
      if (parse_request_line(ln, len, h) < 0) return -1;
      first = 0;
    } else {
      if (*ln == ' ' || *ln == '\t') return -1;     // obsolete line folding
      if (parse_header(ln, len, h) < 0) return -1;
    }
    pos += len + 2;
  }
  if (first) return -1;
  return (int)end;
}

int origin_allowed(const struct http_head *h) {
  if (!h->has_origin) return 1;
  if (strncasecmp(h->origin, "http://", 7) != 0) return 0;
  const char *o = h->origin + 7;

  // host without port; "[v6]:port" keeps its brackets
  size_t hn;
  if (h->host[0] == '[') {
    const char *rb = strchr(h->host, ']');
    hn = rb ? (size_t)(rb - h->host) + 1 : strlen(h->host);
  } else {
    const char *c = strchr(h->host, ':');
    hn = c ? (size_t)(c - h->host) : strlen(h->host);
  }
  if (hn == 0) return 0;
  if (strncasecmp(o, h->host, hn) != 0) return 0;
  const char *rest = o + hn;
  return *rest == 0 || strcmp(rest, ":80") == 0;
}
