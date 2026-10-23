// File storage rules shared by the file commands and print start: /local/ and /usb/ client paths,
// name validation, mount detection and the storage checks ported from the earlier gkweb page server.
#ifndef GKSDCP_FILES_H
#define GKSDCP_FILES_H
#include <stddef.h>

#define FILES_RESERVE_BYTES (16LL * 1024 * 1024)   // free space kept back on an upload

enum { ST_LOCAL = 0, ST_USB = 1 };

// Directories and the mounts file (-d, -U, -M). Trailing slashes are dropped. 0 ok / -1 too long.
int files_init(const char *files_dir, const char *usb_dir, const char *mounts_file);

// 1 if 1..200 bytes of valid UTF-8, no byte < 0x20 or 0x7f, none of / \ : * ? " < > |,
// not starting with '.' (so not . or ..).
int name_valid(const char *name);

// Client path -> absolute path. "/local/..." is the files dir, "/usb/..." the usb dir, no leading '/'
// means "/local/". Every component must pass name_valid; empty components (// or a trailing /) are
// accepted only when the result is an existing directory. 0 ok / -1 invalid / -2 USB not mounted.
int files_resolve(const char *client, char *abs, size_t cap, int *storage);

// Inverse: absolute path under the files or usb dir -> "/local/..." or "/usb/...". 0 ok / -1.
int files_client_path(const char *abs, char *out, size_t cap);

// 1 when the mounts file has an entry whose mount point is the usb dir (read on every call).
int usb_mounted(void);

// Root directory of a storage (as given to files_init, no trailing slash).
const char *files_root(int storage);

// 1 when allow_tmpfs or dir is not on tmpfs/ramfs (a missing dir passes), else 0.
int files_storage_ok(const char *dir, int allow_tmpfs);
// Free bytes for unprivileged writes, -1 on error.
long long files_free_bytes(const char *dir);
// Free space kept back on uploads and USB copies (-R); set once at start-up, default FILES_RESERVE_BYTES.
void files_set_reserve(long long bytes);
long long files_reserve(void);
// 1 when dir has room for bytes more plus the reserve.
int files_space_ok(const char *dir, long long bytes);
#endif
