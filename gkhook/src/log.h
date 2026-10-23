#ifndef GKHOOK_LOG_H
#define GKHOOK_LOG_H
// Appends a line to GKHOOK_LOG (default /tmp/gkhook.log). Errors only, please.
void gk_log(const char* fmt, ...) __attribute__((format(printf, 1, 2), visibility("hidden")));
#endif
