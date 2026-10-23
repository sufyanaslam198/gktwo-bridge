// WebSocket (RFC 6455) server-side framing. No I/O here; loop.c owns the sockets.
#ifndef GKSDCP_WS_H
#define GKSDCP_WS_H
#include <stddef.h>

#define WS_OP_CONT  0
#define WS_OP_TEXT  1
#define WS_OP_BIN   2
#define WS_OP_CLOSE 8
#define WS_OP_PING  9
#define WS_OP_PONG  10

// Sec-WebSocket-Accept value for a client key (28 chars + NUL).
void ws_accept_key(const char *client_key, char out[29]);

struct ws_frame { int fin, op; size_t len; unsigned char *payload; };

// Decode one client frame from buf[0..n). Returns bytes consumed (payload unmasked in place
// and f->payload pointing into buf), 0 if more bytes are needed, -1 on a protocol error:
// unmasked frame, reserved bits set, control frame fragmented or over 125 bytes, or a
// payload over max_payload (rejected as soon as the header is complete).
long ws_decode(unsigned char *buf, size_t n, size_t max_payload, struct ws_frame *f);

// Server frame header (FIN set, unmasked) for op and payload length; returns its size (2, 4 or 10).
size_t ws_header(unsigned char out[10], int op, size_t len);
#endif
