#ifndef GKSDCP_HOOK_IO_H
#define GKSDCP_HOOK_IO_H
#include <stddef.h>

/* Send one JSON line to the control socket, read one reply line (newline stripped,
 * NUL-terminated). The reply must end in a newline and contain no NUL. 0 on success, -1 on connect/IO/timeout error. timeout_ms is total. */
int hook_request(const char *sock_path, const char *json_line, char *reply,
                 size_t reply_len, int timeout_ms);

#endif
