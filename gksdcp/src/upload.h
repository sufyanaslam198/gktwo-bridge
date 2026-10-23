// Chunked file upload: POST /uploadFile/upload (multipart, streamed to disk), the one upload session,
// SDCP Cmd 255 (cancel transfer) and the multipart parser they use.
#ifndef GKSDCP_UPLOAD_H
#define GKSDCP_UPLOAD_H
#include <stddef.h>

// ---- multipart/form-data parser: fixed buffers, bytes may arrive in any pieces ----

#define MP_BOUNDARY_MAX 70
#define MP_HEAD_MAX     1024     // one part's header block
#define MP_NAME_MAX     32
#define MP_FNAME_MAX    256

enum { MP_ERR = -1, MP_ABORT = -2 };   // malformed or out of order / a callback returned non-zero

struct mp_cb {
  // A part's headers are complete. filename is NULL for a plain field; for the file part it is the
  // filename parameter ("" when absent). A part after the file part, or a second file part, is MP_ERR.
  int (*part)(void *u, const char *name, const char *filename);
  int (*data)(void *u, const char *p, size_t n);   // the part's body, in order, any piece size
  int (*end)(void *u);                             // the part's body is complete
  void *u;                                         // each callback returns non-zero to abort
};

struct mp {
  char d[MP_BOUNDARY_MAX + 5];     // "\r\n--" + boundary
  int dl, m;                       // its length; bytes of it matched at the end of the input so far
  int state, rc;
  int after_n;
  char after[2];
  int file_seen;
  int hn;
  char hb[MP_HEAD_MAX];
  char name[MP_NAME_MAX];
  char fname[MP_FNAME_MAX];
  struct mp_cb cb;
};

// 0 ok / -1 when the boundary is empty, over MP_BOUNDARY_MAX bytes or holds a CR or LF.
int mp_init(struct mp *mp, const char *boundary, const struct mp_cb *cb);
// 0 ok so far / MP_ERR / MP_ABORT (sticky). Text before the first boundary and after the closing one is ignored.
int mp_feed(struct mp *mp, const char *p, size_t n);
// 1 once the closing boundary has been seen.
int mp_done(const struct mp *mp);

// ---- upload session ----

// Removes leftover .*.part files in files_dir (once the storage is mounted: at start or from the timer),
// hooks POST /uploadFile/upload, registers Cmd 255 and the stale timer. Call after files_init and status_init,
// before loop_run and before sdcp_init (it installs on_conn_close, which the debug-only test hook in sdcp_init
// chains to). A session with no chunk for stale_seconds is dropped and its temp file removed.
void upload_init(const char *files_dir, int stale_seconds, int allow_tmpfs);
// 1 while an upload keeps print, delete, Z and USB commands out. A session with no chunk in flight and none for
// 5 s is abandoned (DragonFruit never cancels): it is ended here, its temp file removed, and it does not count.
int upload_busy(void);
// 1 when the files folder is on the mounted storage (files_storage_ok with the -T setting).
int upload_storage_ok(void);
// Cmd 255 Acks: 0 cancelled, 1 not transferring, 3 not found (uuid or filename differ; filename may be NULL).
int upload_cancel(const char *uuid, const char *filename);
void upload_tick(void);                                   // drop a stale session
#endif
