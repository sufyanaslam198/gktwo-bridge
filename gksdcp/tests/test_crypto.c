#include "crypto.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CHECK(c) do { if (!(c)) { fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #c); exit(1); } } while (0)

static void hex20(const unsigned char d[20], char out[41]) {
  for (int i = 0; i < 20; i++) sprintf(out + 2 * i, "%02x", d[i]);
}

static void md5_of(const char *s, char out[33]) {
  struct md5 m;
  md5_init(&m);
  md5_update(&m, s, strlen(s));
  md5_hex(&m, out);
}

int main(void) {
  unsigned char d[20];
  char hex[41], b64[64];

  sha1("abc", 3, d); hex20(d, hex);
  CHECK(strcmp(hex, "a9993e364706816aba3e25717850c26c9cd0d89d") == 0);
  sha1("", 0, d); hex20(d, hex);
  CHECK(strcmp(hex, "da39a3ee5e6b4b0d3255bfef95601890afd80709") == 0);
  // 56 bytes: padding spills into a second block
  const char *s56 = "abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq";
  sha1(s56, strlen(s56), d); hex20(d, hex);
  CHECK(strcmp(hex, "84983e441c3bd26ebaae4aa1f95129e5e54670f1") == 0);

  // RFC 6455 section 1.3 handshake
  const char *key = "dGhlIHNhbXBsZSBub25jZQ==258EAFA5-E914-47DA-95CA-C5AB0DC85B11";
  sha1(key, strlen(key), d);
  CHECK(b64_encode(d, 20, b64) == 28);
  CHECK(strcmp(b64, "s3pPLMBiTxaQ9kYGzzhZRbK+xOo=") == 0);
  CHECK(b64_encode((const unsigned char *)"", 0, b64) == 0 && b64[0] == 0);
  CHECK(b64_encode((const unsigned char *)"f", 1, b64) == 4 && strcmp(b64, "Zg==") == 0);
  CHECK(b64_encode((const unsigned char *)"fo", 2, b64) == 4 && strcmp(b64, "Zm8=") == 0);
  CHECK(b64_encode((const unsigned char *)"foo", 3, b64) == 4 && strcmp(b64, "Zm9v") == 0);

  char m[33];
  md5_of("", m);    CHECK(strcmp(m, "d41d8cd98f00b204e9800998ecf8427e") == 0);
  md5_of("abc", m); CHECK(strcmp(m, "900150983cd24fb0d6963f7d28e17f72") == 0);
  md5_of("12345678901234567890123456789012345678901234567890123456789012345678901234567890", m);
  CHECK(strcmp(m, "57edf4a22be3c955ac49da2e2107b67a") == 0);

  // 3 MB fed in uneven pieces equals one-shot
  size_t n = 3u << 20;
  unsigned char *buf = malloc(n);
  CHECK(buf);
  for (size_t i = 0; i < n; i++) buf[i] = (unsigned char)(i * 31 + (i >> 8));
  struct md5 a, b;
  char ha[33], hb[33];
  md5_init(&a); md5_update(&a, buf, n); md5_hex(&a, ha);
  md5_init(&b);
  static const size_t steps[] = {1, 7, 63, 64, 65, 1000, 4093, 65536, 3};
  size_t off = 0;
  for (int i = 0; off < n; i++) {
    size_t k = steps[i % 9];
    if (k > n - off) k = n - off;
    md5_update(&b, buf + off, k);
    off += k;
  }
  md5_hex(&b, hb);
  CHECK(strcmp(ha, hb) == 0);
  free(buf);
  return 0;
}
