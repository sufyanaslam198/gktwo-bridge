// Wire protocol: request parsing and reply building (docs/gkhook-protocol.md).
#ifndef GKHOOK_PROTO_H
#define GKHOOK_PROTO_H
#include <stddef.h>
#include "queue.h"

#define PROTO_MAX 2048  // replies are < PROTO_MAX bytes including '\n'

// 0 ok; -1 malformed/missing field; -2 unknown command. The line may end in \r\n.
int proto_parse(const char* line, size_t n, struct gk_cmd* out);

// Each returns the reply length (ends '\n', no NUL inside), or 0 if it does not fit.
size_t proto_reply_ok(char* buf, size_t cap);
size_t proto_reply_err(char* buf, size_t cap, const char* msg);
size_t proto_reply_status(char* buf, size_t cap, const struct gk_status* st);
#endif
