#include "crypto.h"
#include <string.h>

#define ROL(x, c) (((x) << (c)) | ((x) >> (32 - (c))))

// ---- SHA-1 ----
static void sha1_block(uint32_t h[5], const unsigned char *p) {
  uint32_t w[80];
  for (int i = 0; i < 16; i++)
    w[i] = (uint32_t)p[4 * i] << 24 | (uint32_t)p[4 * i + 1] << 16 |
           (uint32_t)p[4 * i + 2] << 8 | p[4 * i + 3];
  for (int i = 16; i < 80; i++) w[i] = ROL(w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16], 1);
  uint32_t a = h[0], b = h[1], c = h[2], d = h[3], e = h[4];
  for (int i = 0; i < 80; i++) {
    uint32_t f, k;
    if (i < 20)      { f = (b & c) | (~b & d);          k = 0x5a827999; }
    else if (i < 40) { f = b ^ c ^ d;                   k = 0x6ed9eba1; }
    else if (i < 60) { f = (b & c) | (b & d) | (c & d); k = 0x8f1bbcdc; }
    else             { f = b ^ c ^ d;                   k = 0xca62c1d6; }
    uint32_t t = ROL(a, 5) + f + e + k + w[i];
    e = d; d = c; c = ROL(b, 30); b = a; a = t;
  }
  h[0] += a; h[1] += b; h[2] += c; h[3] += d; h[4] += e;
}

void sha1(const void *p, size_t n, unsigned char out[20]) {
  uint32_t h[5] = {0x67452301, 0xefcdab89, 0x98badcfe, 0x10325476, 0xc3d2e1f0};
  const unsigned char *s = p;
  size_t left = n;
  while (left >= 64) { sha1_block(h, s); s += 64; left -= 64; }
  unsigned char tail[128];
  memset(tail, 0, sizeof tail);
  memcpy(tail, s, left);
  tail[left] = 0x80;
  size_t tl = left < 56 ? 64 : 128;
  uint64_t bits = (uint64_t)n * 8;
  for (int i = 0; i < 8; i++) tail[tl - 1 - i] = (unsigned char)(bits >> (8 * i));
  sha1_block(h, tail);
  if (tl == 128) sha1_block(h, tail + 64);
  for (int i = 0; i < 5; i++) {
    out[4 * i] = (unsigned char)(h[i] >> 24); out[4 * i + 1] = (unsigned char)(h[i] >> 16);
    out[4 * i + 2] = (unsigned char)(h[i] >> 8); out[4 * i + 3] = (unsigned char)h[i];
  }
}

// ---- MD5 ----
static const uint32_t MD5_K[64] = {
  0xd76aa478, 0xe8c7b756, 0x242070db, 0xc1bdceee, 0xf57c0faf, 0x4787c62a, 0xa8304613, 0xfd469501,
  0x698098d8, 0x8b44f7af, 0xffff5bb1, 0x895cd7be, 0x6b901122, 0xfd987193, 0xa679438e, 0x49b40821,
  0xf61e2562, 0xc040b340, 0x265e5a51, 0xe9b6c7aa, 0xd62f105d, 0x02441453, 0xd8a1e681, 0xe7d3fbc8,
  0x21e1cde6, 0xc33707d6, 0xf4d50d87, 0x455a14ed, 0xa9e3e905, 0xfcefa3f8, 0x676f02d9, 0x8d2a4c8a,
  0xfffa3942, 0x8771f681, 0x6d9d6122, 0xfde5380c, 0xa4beea44, 0x4bdecfa9, 0xf6bb4b60, 0xbebfbc70,
  0x289b7ec6, 0xeaa127fa, 0xd4ef3085, 0x04881d05, 0xd9d4d039, 0xe6db99e5, 0x1fa27cf8, 0xc4ac5665,
  0xf4292244, 0x432aff97, 0xab9423a7, 0xfc93a039, 0x655b59c3, 0x8f0ccc92, 0xffeff47d, 0x85845dd1,
  0x6fa87e4f, 0xfe2ce6e0, 0xa3014314, 0x4e0811a1, 0xf7537e82, 0xbd3af235, 0x2ad7d2bb, 0xeb86d391};
static const unsigned char MD5_S[64] = {
  7, 12, 17, 22, 7, 12, 17, 22, 7, 12, 17, 22, 7, 12, 17, 22,
  5, 9, 14, 20, 5, 9, 14, 20, 5, 9, 14, 20, 5, 9, 14, 20,
  4, 11, 16, 23, 4, 11, 16, 23, 4, 11, 16, 23, 4, 11, 16, 23,
  6, 10, 15, 21, 6, 10, 15, 21, 6, 10, 15, 21, 6, 10, 15, 21};

static void md5_block(uint32_t s[4], const unsigned char *p) {
  uint32_t m[16];
  for (int i = 0; i < 16; i++)
    m[i] = (uint32_t)p[4 * i] | (uint32_t)p[4 * i + 1] << 8 |
           (uint32_t)p[4 * i + 2] << 16 | (uint32_t)p[4 * i + 3] << 24;
  uint32_t a = s[0], b = s[1], c = s[2], d = s[3];
  for (int i = 0; i < 64; i++) {
    uint32_t f; int g;
    if (i < 16)      { f = (b & c) | (~b & d); g = i; }
    else if (i < 32) { f = (d & b) | (~d & c); g = (5 * i + 1) & 15; }
    else if (i < 48) { f = b ^ c ^ d;          g = (3 * i + 5) & 15; }
    else             { f = c ^ (b | ~d);       g = (7 * i) & 15; }
    f += a + MD5_K[i] + m[g];
    a = d; d = c; c = b; b += ROL(f, MD5_S[i]);
  }
  s[0] += a; s[1] += b; s[2] += c; s[3] += d;
}

void md5_init(struct md5 *m) {
  m->s[0] = 0x67452301; m->s[1] = 0xefcdab89; m->s[2] = 0x98badcfe; m->s[3] = 0x10325476;
  m->bytes = 0;
}

void md5_update(struct md5 *m, const void *p, size_t n) {
  const unsigned char *s = p;
  size_t fill = (size_t)(m->bytes & 63);
  m->bytes += n;
  if (fill) {
    size_t take = 64 - fill < n ? 64 - fill : n;
    memcpy(m->buf + fill, s, take);
    s += take; n -= take;
    if (fill + take < 64) return;
    md5_block(m->s, m->buf);
  }
  while (n >= 64) { md5_block(m->s, s); s += 64; n -= 64; }
  memcpy(m->buf, s, n);
}

void md5_hex(struct md5 *m, char out[33]) {
  uint64_t bits = m->bytes * 8;
  static const unsigned char pad[64] = {0x80};
  size_t fill = (size_t)(m->bytes & 63);
  md5_update(m, pad, fill < 56 ? 56 - fill : 120 - fill);
  unsigned char len[8];
  for (int i = 0; i < 8; i++) len[i] = (unsigned char)(bits >> (8 * i));
  md5_update(m, len, 8);
  static const char hex[] = "0123456789abcdef";
  for (int i = 0; i < 16; i++) {
    unsigned char b = (unsigned char)(m->s[i / 4] >> (8 * (i % 4)));
    out[2 * i] = hex[b >> 4]; out[2 * i + 1] = hex[b & 15];
  }
  out[32] = 0;
}

// ---- base64 ----
size_t b64_encode(const unsigned char *in, size_t n, char *out) {
  static const char T[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
  size_t o = 0;
  for (size_t i = 0; i < n; i += 3) {
    uint32_t v = (uint32_t)in[i] << 16;
    if (i + 1 < n) v |= (uint32_t)in[i + 1] << 8;
    if (i + 2 < n) v |= in[i + 2];
    out[o++] = T[v >> 18];
    out[o++] = T[(v >> 12) & 63];
    out[o++] = i + 1 < n ? T[(v >> 6) & 63] : '=';
    out[o++] = i + 2 < n ? T[v & 63] : '=';
  }
  out[o] = 0;
  return o;
}
