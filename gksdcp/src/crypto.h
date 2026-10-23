// SHA-1, MD5 and base64 (WebSocket handshake, upload checksums).
#ifndef GKSDCP_CRYPTO_H
#define GKSDCP_CRYPTO_H
#include <stddef.h>
#include <stdint.h>

void sha1(const void *p, size_t n, unsigned char out[20]);

struct md5 {
  uint32_t s[4];
  uint64_t bytes;          // total bytes fed so far
  unsigned char buf[64];   // pending partial block
};
void md5_init(struct md5 *m);
void md5_update(struct md5 *m, const void *p, size_t n);
void md5_hex(struct md5 *m, char out[33]);   // finalizes m; lowercase hex + NUL

// Returns the length written; out must hold >= 4*((n+2)/3)+1 bytes, NUL-terminated.
size_t b64_encode(const unsigned char *in, size_t n, char *out);
#endif
