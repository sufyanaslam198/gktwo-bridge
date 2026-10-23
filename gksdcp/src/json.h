// JSON reader (over jsmn) and writer. Reader functions never write outside their out args.
#ifndef GKSDCP_JSON_H
#define GKSDCP_JSON_H
#include <stddef.h>
#define JSMN_STRICT
#define JSMN_HEADER   // declarations only; json.c holds the one implementation
#include "jsmn.h"
#undef JSMN_HEADER

struct jdoc { const char *js; jsmntok_t *t; int n; };

// Parse js[0..len) into toks[0..max). 0 ok / -1 (invalid, truncated or too many tokens).
int jdoc_parse(struct jdoc *d, const char *js, size_t len, jsmntok_t *toks, int max);
// Number of tokens that js needs, or -1 when it is not valid JSON.
int jdoc_count(const char *js, size_t len);
// Value token of key in object token obj; -1 if absent or obj is not an object.
int jget(const struct jdoc *d, int obj, const char *key);
// Value token at a dotted path from the root, e.g. "Data.Data.Filename"; -1 if absent.
int jpath(const struct jdoc *d, const char *dotted);
// Decode a string token (escapes, \uXXXX to UTF-8). 0 ok / -1 not a string, too long
// for cap (NUL included), or contains NUL.
int jstr(const struct jdoc *d, int tok, char *out, size_t cap);
// Integer literal only (no fraction, exponent, quotes). 0 ok / -1.
int jint(const struct jdoc *d, int tok, long long *out);
int jarr_n(const struct jdoc *d, int tok);            // element count, -1 if not an array
int jarr_at(const struct jdoc *d, int tok, int i);    // token index of element i, -1 if out of range
// Token after the value at token i: the next element when i is an array element. A loop over an array steps
// with jnext from tok + 1 (linear); jarr_at in a loop is quadratic.
int jnext(const struct jdoc *d, int i);

// Writer into a caller buffer. On overflow err is set, nothing past cap is written
// and the buffer stays NUL-terminated.
struct jw { char *b; size_t cap, len; int err; };
void jw_init(struct jw *w, char *buf, size_t cap);
void jw_raw(struct jw *w, const char *s);
void jw_str(struct jw *w, const char *utf8);          // quoted and escaped
void jw_int(struct jw *w, long long v);
void jw_dbl(struct jw *w, double v, int decimals);
void jw_bool(struct jw *w, int v);
#endif
