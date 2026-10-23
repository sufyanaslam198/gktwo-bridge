// Unix-socket server thread (off the UI thread).
#ifndef GKHOOK_SERVER_H
#define GKHOOK_SERVER_H
#include <stddef.h>
#include <stdint.h>

// Validators for `start`. NULL = pass. Return 0 ok, else fill err (short message).
// cheap runs on the server thread; full runs on a detached checker thread.
struct gk_start_ops {
  int (*cheap)(const char* path, char* err, size_t errn);
  int (*full)(const char* path, char* err, size_t errn);
  // Checker thread, after full: reads + validates the file magic carried to the tick (NULL = 0).
  int (*magic)(const char* path, uint32_t* magic, char* err, size_t errn);
};
extern struct gk_start_ops gk_start_ops __attribute__((visibility("hidden")));

// Spawns the detached server thread. Bind/listen errors are logged from the thread.
// Returns 0 if the thread was created, -1 otherwise. Never blocks.
int gk_server_start(const char* sock_path);
#endif
