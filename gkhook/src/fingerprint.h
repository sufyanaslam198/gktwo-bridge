// V1.3.3 chitu fingerprint check: file size, then byte windows via peek.
#ifndef GKHOOK_FINGERPRINT_H
#define GKHOOK_FINGERPRINT_H
#include <stddef.h>
#include <stdint.h>

// Returns 1 iff exe_path has the expected size and every byte window matches.
// peek reads n bytes at addr into out and returns 0 on success.
int verify_fingerprint(const char* exe_path,
                       int (*peek)(uintptr_t addr, unsigned char* out, size_t n));
#endif
