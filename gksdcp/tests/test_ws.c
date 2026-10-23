#include "ws.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CHECK(c) do { if (!(c)) { fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #c); exit(1); } } while (0)

// Build a masked client frame; returns its length.
static size_t mk(unsigned char *out, int op, int fin, const unsigned char *p, size_t n, int masked) {
  size_t i = 0;
  const unsigned char key[4] = {0x37, 0xfa, 0x21, 0x3d};
  out[i++] = (unsigned char)((fin ? 0x80 : 0) | op);
  unsigned char m = masked ? 0x80 : 0;
  if (n < 126) out[i++] = (unsigned char)(m | n);
  else if (n < 65536) { out[i++] = m | 126; out[i++] = (unsigned char)(n >> 8); out[i++] = (unsigned char)n; }
  else { out[i++] = m | 127; for (int s = 56; s >= 0; s -= 8) out[i++] = (unsigned char)((unsigned long long)n >> s); }
  if (masked) { memcpy(out + i, key, 4); i += 4; }
  for (size_t k = 0; k < n; k++) out[i + k] = masked ? (p[k] ^ key[k & 3]) : p[k];
  return i + n;
}

int main(void) {
  char key[29];
  ws_accept_key("dGhlIHNhbXBsZSBub25jZQ==", key);
  CHECK(strcmp(key, "s3pPLMBiTxaQ9kYGzzhZRbK+xOo=") == 0);

  // RFC 6455 section 5.7: masked "Hello"
  unsigned char hello[] = {0x81, 0x85, 0x37, 0xfa, 0x21, 0x3d, 0x7f, 0x9f, 0x4d, 0x51, 0x58};
  struct ws_frame f;
  CHECK(ws_decode(hello, 5, 65536, &f) == 0);          // partial
  CHECK(ws_decode(hello, sizeof hello - 1, 65536, &f) == 0);
  CHECK(ws_decode(hello, sizeof hello, 65536, &f) == (long)sizeof hello);
  CHECK(f.fin == 1 && f.op == 1 && f.len == 5 && memcmp(f.payload, "Hello", 5) == 0);

  static unsigned char buf[70000 + 32], pl[70000];
  memset(pl, 'x', sizeof pl);

  // unmasked client frame is a protocol error
  size_t n = mk(buf, 1, 1, pl, 5, 0);
  CHECK(ws_decode(buf, n, 65536, &f) == -1);

  // 126 form
  n = mk(buf, 2, 1, pl, 300, 1);
  CHECK(ws_decode(buf, n, 65536, &f) == (long)n);
  CHECK(f.op == 2 && f.len == 300 && f.payload[299] == 'x');
  // 127 form
  n = mk(buf, 2, 1, pl, 65536, 1);
  CHECK(ws_decode(buf, n, 65536, &f) == (long)n);
  CHECK(f.len == 65536);
  // over max: rejected as soon as the header is in, payload not needed
  n = mk(buf, 2, 1, pl, 65537, 1);
  CHECK(ws_decode(buf, 14, 65536, &f) == -1);
  // a 64-bit length with the top bit set is an error
  unsigned char huge[] = {0x82, 0xff, 0x80, 0, 0, 0, 0, 0, 0, 0, 1, 2, 3, 4};
  CHECK(ws_decode(huge, sizeof huge, 65536, &f) == -1);

  // reserved bits
  n = mk(buf, 1, 1, pl, 3, 1);
  buf[0] |= 0x40;
  CHECK(ws_decode(buf, n, 65536, &f) == -1);

  // ping with 125 ok, 126 rejected, fragmented ping rejected
  n = mk(buf, 9, 1, pl, 125, 1);
  CHECK(ws_decode(buf, n, 65536, &f) == (long)n && f.op == 9);
  n = mk(buf, 9, 1, pl, 126, 1);
  CHECK(ws_decode(buf, n, 65536, &f) == -1);
  n = mk(buf, 9, 0, pl, 4, 1);
  CHECK(ws_decode(buf, n, 65536, &f) == -1);

  // two frames back to back: first one consumed only
  size_t a = mk(buf, 1, 1, (const unsigned char *)"ab", 2, 1);
  size_t b = mk(buf + a, 1, 1, (const unsigned char *)"cd", 2, 1);
  CHECK(ws_decode(buf, a + b, 65536, &f) == (long)a);
  CHECK(memcmp(f.payload, "ab", 2) == 0);

  // server headers
  unsigned char h[10];
  CHECK(ws_header(h, 1, 5) == 2 && h[0] == 0x81 && h[1] == 5);
  CHECK(ws_header(h, 10, 125) == 2 && h[0] == 0x8a);
  CHECK(ws_header(h, 1, 126) == 4 && h[1] == 126 && h[2] == 0 && h[3] == 126);
  CHECK(ws_header(h, 2, 65535) == 4 && h[2] == 0xff && h[3] == 0xff);
  CHECK(ws_header(h, 1, 65536) == 10 && h[1] == 127 && h[7] == 1 && h[8] == 0 && h[9] == 0);
  return 0;
}
