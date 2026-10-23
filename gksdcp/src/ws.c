#include "ws.h"
#include "crypto.h"
#include <string.h>

void ws_accept_key(const char *client_key, char out[29]) {
  static const char guid[] = "258EAFA5-E914-47DA-95CA-C5AB0DC85B11";
  char in[64 + sizeof guid];
  size_t kn = strnlen(client_key, 64);
  memcpy(in, client_key, kn);
  memcpy(in + kn, guid, sizeof guid - 1);
  unsigned char d[20];
  sha1(in, kn + sizeof guid - 1, d);
  b64_encode(d, 20, out);             // 28 chars + NUL
}

long ws_decode(unsigned char *buf, size_t n, size_t max_payload, struct ws_frame *f) {
  if (n < 2) return 0;
  if (buf[0] & 0x70) return -1;                       // reserved bits
  int fin = buf[0] >> 7, op = buf[0] & 0x0F;
  if (!(buf[1] & 0x80)) return -1;                    // clients must mask
  unsigned long long len = buf[1] & 0x7F;
  size_t off = 2;
  if (len == 126) {
    if (n < 4) return 0;
    len = (unsigned long long)buf[2] << 8 | buf[3];
    off = 4;
  } else if (len == 127) {
    if (n < 10) return 0;
    len = 0;
    for (int i = 0; i < 8; i++) len = len << 8 | buf[2 + i];
    if (len >> 63) return -1;
    off = 10;
  }
  if ((op & 0x8) && (!fin || len > 125)) return -1;   // control frames: unfragmented, short
  if (len > max_payload) return -1;
  if (n < off + 4 + len) return 0;
  const unsigned char *key = buf + off;
  unsigned char *p = buf + off + 4;
  for (size_t i = 0; i < len; i++) p[i] ^= key[i & 3];
  f->fin = fin; f->op = op; f->len = (size_t)len; f->payload = p;
  return (long)(off + 4 + len);
}

size_t ws_header(unsigned char out[10], int op, size_t len) {
  out[0] = (unsigned char)(0x80 | op);
  if (len < 126) { out[1] = (unsigned char)len; return 2; }
  if (len < 65536) { out[1] = 126; out[2] = (unsigned char)(len >> 8); out[3] = (unsigned char)len; return 4; }
  out[1] = 127;
  for (int i = 0; i < 8; i++) out[2 + i] = (unsigned char)((unsigned long long)len >> (56 - 8 * i));
  return 10;
}
