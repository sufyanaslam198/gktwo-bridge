// .ctb validation for `start` (off the UI thread; print_file_* and the resolution globals
// are allowed here per R6). Return 0 ok, else a short message in err.
#ifndef GKHOOK_VALIDATE_H
#define GKHOOK_VALIDATE_H
#include <stddef.h>
#include <stdint.h>

// Fast checks, run by the server before it accepts `start`.
int gk_validate_cheap(const char* path, char* err, size_t errn) __attribute__((visibility("hidden")));
// Slow whole-file check (print_file_check), run on the detached checker thread.
int gk_validate_full(const char* path, char* err, size_t errn) __attribute__((visibility("hidden")));
// Reads and checks the CTB magic (0x12fd0106/07); carried to the tick. 0 ok.
int gk_validate_magic(const char* path, uint32_t* magic, char* err, size_t errn) __attribute__((visibility("hidden")));
// First 4 bytes of the file, little-endian (the item magic). 0 ok, -1 unreadable.
int gk_file_magic(const char* path, uint32_t* out) __attribute__((visibility("hidden")));
#endif
