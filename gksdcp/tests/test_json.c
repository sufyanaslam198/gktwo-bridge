#include "json.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CHECK(c) do { if (!(c)) { fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #c); exit(1); } } while (0)

static jsmntok_t toks[64];

static int parse(struct jdoc *d, const char *js) { return jdoc_parse(d, js, strlen(js), toks, 64); }

static int str_tok(struct jdoc *d, const char *js, char *out, size_t cap) {
  if (parse(d, js) < 0) return -1;   // jsmn already rejects some bad escapes
  return jstr(d, jarr_at(d, 0, 0), out, cap);
}

int main(void) {
  struct jdoc d;
  char s[128];
  long long v;

  // a DragonFruit "list /local/" request (Cmd 258)
  const char *req =
    "{\"Id\":\"a0b1c2d3e4f5a6b7c8d9e0f1a2b3c4d5\",\"Data\":{\"Cmd\":258,"
    "\"Data\":{\"Url\":\"/local/\"},\"RequestID\":\"0123456789abcdef0123456789abcdef\","
    "\"MainboardID\":\"0000020000aabbcc\",\"TimeStamp\":1700000000,\"From\":0},\"Extra\":[1,[2,3],{\"k\":4}]}";
  CHECK(parse(&d, req) == 0);
  int t = jpath(&d, "Data.Data.Url");
  CHECK(t >= 0 && jstr(&d, t, s, sizeof s) == 0 && strcmp(s, "/local/") == 0);
  CHECK(jint(&d, jpath(&d, "Data.Cmd"), &v) == 0 && v == 258);
  CHECK(jint(&d, jpath(&d, "Data.TimeStamp"), &v) == 0 && v == 1700000000);
  CHECK(jstr(&d, jpath(&d, "Data.RequestID"), s, sizeof s) == 0 && strlen(s) == 32);
  CHECK(jpath(&d, "Data.Data.Nope") == -1);
  CHECK(jpath(&d, "Data.Cmd.X") == -1);        // descending into a non-object
  CHECK(jpath(&d, "Data.Url") == -1);
  CHECK(jget(&d, jpath(&d, "Data.Cmd"), "x") == -1);
  CHECK(jget(&d, 0, "Id") >= 0);
  // a key after a nested subtree is still found
  int ex = jget(&d, 0, "Extra");
  CHECK(ex >= 0 && jarr_n(&d, ex) == 3);
  CHECK(jint(&d, jarr_at(&d, ex, 0), &v) == 0 && v == 1);
  CHECK(jarr_n(&d, jarr_at(&d, ex, 1)) == 2);
  CHECK(jint(&d, jget(&d, jarr_at(&d, ex, 2), "k"), &v) == 0 && v == 4);
  CHECK(jarr_at(&d, ex, 3) == -1 && jarr_at(&d, ex, -1) == -1);
  CHECK(jarr_n(&d, 0) == -1);

  // garbage never parses
  CHECK(parse(&d, "not json") == -1);
  CHECK(parse(&d, "{\"a\":") == -1);
  CHECK(parse(&d, "") == -1);
  CHECK(jdoc_parse(&d, req, strlen(req), toks, 5) == -1);   // too few tokens

  // string decoding: raw UTF-8, \u escapes, surrogate pair
  CHECK(str_tok(&d, "[\"\xc3\x9c\xf0\x9f\x98\x80\"]", s, sizeof s) == 0 && strcmp(s, "\xc3\x9c\xf0\x9f\x98\x80") == 0);
  CHECK(str_tok(&d, "[\"\\u00dc\\ud83d\\ude00\"]", s, sizeof s) == 0 && strcmp(s, "\xc3\x9c\xf0\x9f\x98\x80") == 0);
  CHECK(str_tok(&d, "[\"a\\\"b\\\\c\\/d\\n\\t\\u20ac\"]", s, sizeof s) == 0 && strcmp(s, "a\"b\\c/d\n\t\xe2\x82\xac") == 0);
  CHECK(str_tok(&d, "[\"\\u0000\"]", s, sizeof s) == -1);
  CHECK(str_tok(&d, "[\"a\\u0000b\"]", s, sizeof s) == -1);
  CHECK(str_tok(&d, "[\"\\ud83d\"]", s, sizeof s) == -1);          // lone high surrogate
  CHECK(str_tok(&d, "[\"\\ude00\"]", s, sizeof s) == -1);          // lone low surrogate
  CHECK(str_tok(&d, "[\"\\ud83dx\"]", s, sizeof s) == -1);
  CHECK(str_tok(&d, "[\"\\q\"]", s, sizeof s) == -1);
  CHECK(str_tok(&d, "[\"\\u12\"]", s, sizeof s) == -1);
  CHECK(str_tok(&d, "[\"abcd\"]", s, 5) == 0 && strcmp(s, "abcd") == 0);   // exactly fits
  CHECK(str_tok(&d, "[\"abcd\"]", s, 4) == -1);                            // no room for NUL
  CHECK(str_tok(&d, "[\"\\u20ac\"]", s, 3) == -1);                         // multibyte needs 3+NUL
  CHECK(str_tok(&d, "[\"\"]", s, 1) == 0 && s[0] == 0);
  CHECK(str_tok(&d, "[5]", s, sizeof s) == -1);                            // not a string

  // jint accepts integer literals only
  CHECK(parse(&d, "[0,-7,9223372036854775807,-9223372036854775808]") == 0);
  CHECK(jint(&d, 1, &v) == 0 && v == 0);
  CHECK(jint(&d, 2, &v) == 0 && v == -7);
  CHECK(jint(&d, 3, &v) == 0 && v == 9223372036854775807LL);
  CHECK(jint(&d, 4, &v) == 0 && v == (-9223372036854775807LL - 1));
  const char *bad[] = {"[1.5]", "[\"5\"]", "[1e3]", "[true]", "[null]", "[-]", "[01]",
                       "[9223372036854775808]", "[-9223372036854775809]", "[99999999999999999999]"};
  for (size_t i = 0; i < sizeof bad / sizeof *bad; i++) {
    if (parse(&d, bad[i]) < 0) continue;   // strict jsmn may already reject it
    CHECK(jint(&d, 1, &v) == -1);
  }
  CHECK(parse(&d, "[1.5]") == 0 && jint(&d, 1, &v) == -1);
  CHECK(parse(&d, "[\"5\"]") == 0 && jint(&d, 1, &v) == -1);
  CHECK(parse(&d, "[1e3]") == 0 && jint(&d, 1, &v) == -1);

  // writer round-trip with quote, backslash, control bytes and UTF-8
  const char *nasty = "q\"b\\n\n\r\t\b\f\x01\x1f \xc3\x9c/";
  char out[256];
  struct jw w;
  jw_init(&w, out, sizeof out);
  jw_raw(&w, "{\"k\":"); jw_str(&w, nasty);
  jw_raw(&w, ",\"i\":"); jw_int(&w, -42);
  jw_raw(&w, ",\"d\":"); jw_dbl(&w, 3.14159, 2);
  jw_raw(&w, ",\"b\":"); jw_bool(&w, 1);
  jw_raw(&w, ",\"f\":"); jw_bool(&w, 0);
  jw_raw(&w, "}");
  CHECK(!w.err && w.len == strlen(out));
  CHECK(parse(&d, out) == 0);
  CHECK(jstr(&d, jget(&d, 0, "k"), s, sizeof s) == 0 && strcmp(s, nasty) == 0);
  CHECK(jint(&d, jget(&d, 0, "i"), &v) == 0 && v == -42);
  CHECK(strstr(out, "\"d\":3.14,") != NULL);
  CHECK(strstr(out, "\"b\":true,\"f\":false") != NULL);
  jw_init(&w, out, sizeof out);
  jw_dbl(&w, NAN, 3); CHECK(strcmp(out, "0.000") == 0);

  // overflow: err set, never past cap, always NUL-terminated
  for (size_t cap = 1; cap < 12; cap++) {
    char guard[32];
    memset(guard, 'X', sizeof guard);
    jw_init(&w, guard + 8, cap);
    jw_raw(&w, "hello"); jw_str(&w, "world"); jw_int(&w, 123456);
    CHECK(w.err);
    CHECK(memchr(guard + 8, 0, cap) != NULL);
    CHECK(strlen(guard + 8) < cap);
    for (int i = 0; i < 8; i++) CHECK(guard[i] == 'X');
    for (size_t i = 8 + cap; i < sizeof guard; i++) CHECK(guard[i] == 'X');
  }
  char tiny[6];
  jw_init(&w, tiny, sizeof tiny);
  jw_raw(&w, "12345"); CHECK(!w.err && strcmp(tiny, "12345") == 0);   // exactly fits
  jw_raw(&w, "6"); CHECK(w.err && strcmp(tiny, "12345") == 0);
  jw_raw(&w, ""); CHECK(strcmp(tiny, "12345") == 0);                  // stays failed
  jw_init(&w, tiny, 0); jw_raw(&w, "x"); CHECK(w.err);                // cap 0 writes nothing
  return 0;
}
