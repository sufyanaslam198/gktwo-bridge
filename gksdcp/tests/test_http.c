#include "http.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CHECK(c) do { if (!(c)) { fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #c); exit(1); } } while (0)

static int origin_ok(const char *host, const char *origin) {
  char req[512];
  struct http_head h;
  int n = snprintf(req, sizeof req, "GET /websocket HTTP/1.1\r\nHost: %s\r\n%s%s%sUpgrade: websocket\r\n\r\n",
                   host, origin ? "Origin: " : "", origin ? origin : "", origin ? "\r\n" : "");
  CHECK(http_parse_head(req, (size_t)n, &h) == n);
  return origin_allowed(&h);
}

int main(void) {
  struct http_head h;

  // head split across three buffers
  const char *req = "GET /websocket HTTP/1.1\r\nHost: 192.168.1.5:3030\r\nUpgrade: WebSocket\r\n"
                    "Connection: Upgrade\r\nSec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\n\r\n";
  size_t len = strlen(req);
  CHECK(http_parse_head(req, 20, &h) == 0);
  CHECK(http_parse_head(req, len - 2, &h) == 0);
  CHECK(http_parse_head(req, len, &h) == (int)len);
  CHECK(strcmp(h.method, "GET") == 0);
  CHECK(strcmp(h.path, "/websocket") == 0);
  CHECK(strcmp(h.host, "192.168.1.5:3030") == 0);
  CHECK(strcmp(h.ws_key, "dGhlIHNhbXBsZSBub25jZQ==") == 0);
  CHECK(h.upgrade_ws == 1 && h.has_origin == 0 && h.has_te == 0 && h.clen == 0);

  // bytes after the head are not counted
  char two[512];
  snprintf(two, sizeof two, "%sEXTRA", req);
  CHECK(http_parse_head(two, strlen(two), &h) == (int)len);

  // upload head fields
  const char *up = "POST /uploadFile/upload HTTP/1.1\r\nhost: x\r\ncontent-length: 1234567\r\n"
                   "Content-Type: multipart/form-data; boundary=abc\r\nTransfer-Encoding: chunked\r\n\r\n";
  CHECK(http_parse_head(up, strlen(up), &h) == (int)strlen(up));
  CHECK(strcmp(h.method, "POST") == 0 && strcmp(h.path, "/uploadFile/upload") == 0);
  CHECK(h.clen == 1234567 && h.has_te == 1 && h.upgrade_ws == 0);
  CHECK(strcmp(h.ctype, "multipart/form-data; boundary=abc") == 0);

  // malformed
  CHECK(http_parse_head("GARBAGE\r\n\r\n", 11, &h) == -1);
  const char *nocolon = "GET / HTTP/1.1\r\nBadHeaderNoColon\r\n\r\n";
  CHECK(http_parse_head(nocolon, strlen(nocolon), &h) == -1);
  const char *badcl = "POST / HTTP/1.1\r\nContent-Length: 12x\r\n\r\n";
  CHECK(http_parse_head(badcl, strlen(badcl), &h) == -1);
  const char *negcl = "POST / HTTP/1.1\r\nContent-Length: -5\r\n\r\n";
  CHECK(http_parse_head(negcl, strlen(negcl), &h) == -1);

  // head larger than 8 KB is malformed, with or without a terminator
  static char big[9000];
  memset(big, 'a', sizeof big);
  memcpy(big, "GET / HTTP/1.1\r\nX: ", 19);
  CHECK(http_parse_head(big, 8192, &h) == -1);
  memcpy(big + 8990, "\r\n\r\n", 4);
  CHECK(http_parse_head(big, 8994, &h) == -1);

  // Origin rule
  CHECK(origin_ok("192.168.1.5:3030", "http://192.168.1.5") == 1);
  CHECK(origin_ok("192.168.1.5:3030", "http://192.168.1.5:80") == 1);
  CHECK(origin_ok("192.168.1.5", "http://192.168.1.5") == 1);
  CHECK(origin_ok("GKtwo.local:3030", "http://gktwo.LOCAL") == 1);
  CHECK(origin_ok("192.168.1.5:3030", "http://evil.example") == 0);
  CHECK(origin_ok("192.168.1.5:3030", "null") == 0);
  CHECK(origin_ok("192.168.1.5:3030", "https://192.168.1.5") == 0);
  CHECK(origin_ok("192.168.1.5:3030", "http://192.168.1.5:3030") == 0);
  CHECK(origin_ok("192.168.1.5:3030", "http://192.168.1.50") == 0);
  CHECK(origin_ok("192.168.1.5:3030", NULL) == 1);
  return 0;
}
