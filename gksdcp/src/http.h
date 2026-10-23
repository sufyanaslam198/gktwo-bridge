// Incremental HTTP request-head parser and the same-host Origin rule.
#ifndef GKSDCP_HTTP_H
#define GKSDCP_HTTP_H
#include <stddef.h>

#define HTTP_MAX_HEAD 8192

struct http_head {
  char method[8];
  char path[256];
  char host[128];
  char origin[160];
  char ctype[160];
  char ws_key[64];
  long long clen;                       // Content-Length, 0 when absent
  int has_origin, upgrade_ws, has_te;   // Origin present; Upgrade: websocket; Transfer-Encoding present
};

// Returns the head length including the final CRLFCRLF, 0 if more bytes are needed,
// -1 if malformed (bad request line or header, a field too long for its slot, a bad
// Content-Length, or a head over HTTP_MAX_HEAD). Bytes after the head are not touched.
int http_parse_head(const char *buf, size_t n, struct http_head *h);

// 1 if there is no Origin, or Origin is http://<host without port> or the same with :80
// (host compared case-insensitively); 0 otherwise.
int origin_allowed(const struct http_head *h);
#endif
