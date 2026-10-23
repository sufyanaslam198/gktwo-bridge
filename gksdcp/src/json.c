#define JSMN_STRICT
#include "jsmn.h"   // before json.h: this TU carries the jsmn implementation
#include "json.h"
#include <stdio.h>
#include <string.h>

int jdoc_parse(struct jdoc *d, const char *js, size_t len, jsmntok_t *toks, int max) {
  jsmn_parser p;
  jsmn_init(&p);
  int r = jsmn_parse(&p, js, len, toks, (unsigned)max);
  if (r < 1) return -1;
  d->js = js; d->t = toks; d->n = r;
  return 0;
}

int jdoc_count(const char *js, size_t len) {
  jsmn_parser p;
  jsmn_init(&p);
  int r = jsmn_parse(&p, js, len, NULL, 0);
  return r < 1 ? -1 : r;
}

static int tok_ok(const struct jdoc *d, int i) { return i >= 0 && i < d->n; }

// Index of the token after the subtree rooted at i.
static int skip(const struct jdoc *d, int i) {
  int remaining = 1;
  while (remaining > 0 && i < d->n) { remaining += d->t[i].size - 1; i++; }
  return i;
}

static int key_eq(const struct jdoc *d, int k, const char *key, size_t klen) {
  const jsmntok_t *t = &d->t[k];
  if (t->type != JSMN_STRING) return 0;
  size_t n = (size_t)(t->end - t->start);
  if (!memchr(d->js + t->start, '\\', n)) return n == klen && memcmp(d->js + t->start, key, n) == 0;
  char buf[128];   // escaped key: compare the decoded form (longer keys never match)
  if (jstr(d, k, buf, sizeof buf) < 0) return 0;
  return strlen(buf) == klen && memcmp(buf, key, klen) == 0;
}

static int jget_n(const struct jdoc *d, int obj, const char *key, size_t klen) {
  if (!tok_ok(d, obj) || d->t[obj].type != JSMN_OBJECT) return -1;
  int i = obj + 1;
  for (int k = 0; k < d->t[obj].size; k++) {
    if (i + 1 >= d->n) return -1;
    if (key_eq(d, i, key, klen)) return i + 1;
    i = skip(d, i + 1);
  }
  return -1;
}

int jget(const struct jdoc *d, int obj, const char *key) { return jget_n(d, obj, key, strlen(key)); }

int jpath(const struct jdoc *d, const char *dotted) {
  if (d->n < 1) return -1;
  int cur = 0;
  for (const char *s = dotted;;) {
    const char *dot = strchr(s, '.');
    size_t n = dot ? (size_t)(dot - s) : strlen(s);
    cur = jget_n(d, cur, s, n);
    if (cur < 0 || !dot) return cur;
    s = dot + 1;
  }
}

static int hex4(const char *p, const char *end, unsigned *out) {
  if (end - p < 4) return -1;
  unsigned v = 0;
  for (int i = 0; i < 4; i++) {
    char c = p[i];
    v <<= 4;
    if (c >= '0' && c <= '9') v |= (unsigned)(c - '0');
    else if (c >= 'a' && c <= 'f') v |= (unsigned)(c - 'a' + 10);
    else if (c >= 'A' && c <= 'F') v |= (unsigned)(c - 'A' + 10);
    else return -1;
  }
  *out = v;
  return 0;
}

int jstr(const struct jdoc *d, int tok, char *out, size_t cap) {
  if (!tok_ok(d, tok) || d->t[tok].type != JSMN_STRING || cap == 0) return -1;
  const char *p = d->js + d->t[tok].start, *end = d->js + d->t[tok].end;
  size_t o = 0;
  while (p < end) {
    unsigned cp;
    if (*p != '\\') {   // raw byte (incl. UTF-8 sequences) is copied through
      if (*p == 0 || o + 1 >= cap) return -1;
      out[o++] = *p++;
      continue;
    } else {
      if (++p >= end) return -1;
      char c = *p++;
      switch (c) {
        case '"': cp = '"'; break;   case '\\': cp = '\\'; break; case '/': cp = '/'; break;
        case 'b': cp = '\b'; break;  case 'f': cp = '\f'; break;  case 'n': cp = '\n'; break;
        case 'r': cp = '\r'; break;  case 't': cp = '\t'; break;
        case 'u':
          if (hex4(p, end, &cp) < 0) return -1;
          p += 4;
          if (cp >= 0xd800 && cp < 0xdc00) {   // a high surrogate needs its low half
            unsigned lo;
            if (end - p < 6 || p[0] != '\\' || p[1] != 'u' || hex4(p + 2, end, &lo) < 0 ||
                lo < 0xdc00 || lo > 0xdfff) return -1;
            p += 6;
            cp = 0x10000 + ((cp - 0xd800) << 10) + (lo - 0xdc00);
          } else if (cp >= 0xdc00 && cp <= 0xdfff) {
            return -1;   // lone low surrogate
          }
          break;
        default: return -1;
      }
    }
    if (cp == 0) return -1;
    size_t need = cp < 0x80 ? 1 : cp < 0x800 ? 2 : cp < 0x10000 ? 3 : 4;
    if (o + need >= cap) return -1;
    if (need == 1) out[o++] = (char)cp;
    else if (need == 2) { out[o++] = (char)(0xc0 | cp >> 6); out[o++] = (char)(0x80 | (cp & 63)); }
    else if (need == 3) { out[o++] = (char)(0xe0 | cp >> 12); out[o++] = (char)(0x80 | ((cp >> 6) & 63)); out[o++] = (char)(0x80 | (cp & 63)); }
    else { out[o++] = (char)(0xf0 | cp >> 18); out[o++] = (char)(0x80 | ((cp >> 12) & 63)); out[o++] = (char)(0x80 | ((cp >> 6) & 63)); out[o++] = (char)(0x80 | (cp & 63)); }
  }
  out[o] = 0;
  return 0;
}

int jint(const struct jdoc *d, int tok, long long *out) {
  if (!tok_ok(d, tok) || d->t[tok].type != JSMN_PRIMITIVE) return -1;
  const char *p = d->js + d->t[tok].start, *end = d->js + d->t[tok].end;
  int neg = 0;
  if (p < end && *p == '-') { neg = 1; p++; }
  if (p == end || *p < '0' || *p > '9') return -1;
  if (*p == '0' && end - p > 1) return -1;   // no leading zeros
  unsigned long long v = 0, lim = neg ? 9223372036854775808ULL : 9223372036854775807ULL;
  for (; p < end; p++) {
    if (*p < '0' || *p > '9') return -1;
    unsigned dg = (unsigned)(*p - '0');
    if (v > (lim - dg) / 10) return -1;
    v = v * 10 + dg;
  }
  *out = neg ? (long long)(0 - v) : (long long)v;
  return 0;
}

int jarr_n(const struct jdoc *d, int tok) {
  return tok_ok(d, tok) && d->t[tok].type == JSMN_ARRAY ? d->t[tok].size : -1;
}

int jnext(const struct jdoc *d, int i) { return skip(d, i); }

int jarr_at(const struct jdoc *d, int tok, int i) {
  int n = jarr_n(d, tok);
  if (n < 0 || i < 0 || i >= n) return -1;
  int j = tok + 1;
  while (i-- > 0) j = skip(d, j);
  return j < d->n ? j : -1;
}

// ---- writer ----
void jw_init(struct jw *w, char *buf, size_t cap) {
  w->b = buf; w->cap = cap; w->len = 0; w->err = 0;
  if (cap) buf[0] = 0; else w->err = 1;
}

static void put(struct jw *w, const char *s, size_t n) {
  if (w->err) return;
  if (w->len + n + 1 > w->cap) { w->err = 1; return; }
  memcpy(w->b + w->len, s, n);
  w->len += n;
  w->b[w->len] = 0;
}

void jw_raw(struct jw *w, const char *s) { put(w, s, strlen(s)); }

void jw_str(struct jw *w, const char *s) {
  put(w, "\"", 1);
  for (; *s; s++) {
    unsigned char c = (unsigned char)*s;
    char e[8];
    switch (c) {
      case '"':  put(w, "\\\"", 2); break;
      case '\\': put(w, "\\\\", 2); break;
      case '\n': put(w, "\\n", 2); break;
      case '\r': put(w, "\\r", 2); break;
      case '\t': put(w, "\\t", 2); break;
      case '\b': put(w, "\\b", 2); break;
      case '\f': put(w, "\\f", 2); break;
      default:
        if (c < 0x20) { snprintf(e, sizeof e, "\\u%04x", c); put(w, e, 6); }
        else put(w, s, 1);
    }
  }
  put(w, "\"", 1);
}

void jw_int(struct jw *w, long long v) {
  char b[32];
  int n = snprintf(b, sizeof b, "%lld", v);
  put(w, b, (size_t)n);
}

void jw_dbl(struct jw *w, double v, int decimals) {
  char b[352];   // %f of DBL_MAX plus 15 decimals fits
  if (decimals < 0) decimals = 0;
  if (decimals > 15) decimals = 15;
  if (v != v || v > 1.7976931348623157e308 || v < -1.7976931348623157e308) v = 0;   // no NaN/inf in JSON
  int n = snprintf(b, sizeof b, "%.*f", decimals, v);
  put(w, b, (size_t)n);
}

void jw_bool(struct jw *w, int v) { jw_raw(w, v ? "true" : "false"); }
