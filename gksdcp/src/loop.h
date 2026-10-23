// Single-threaded poll() event loop: TCP listener, HTTP head handling, WebSocket connections.
#ifndef GKSDCP_LOOP_H
#define GKSDCP_LOOP_H
#include <stddef.h>
#include <stdint.h>
#include "http.h"

#define MAX_CONNS        32
#define MAX_WS_PAYLOAD   65536
#define HEAD_DEADLINE_MS 10000
#define WS_IDLE_MS       120000
#define UPLOAD_IDLE_MS   30000
#define SEND_QUEUE_CAP   (512 * 1024)

struct gk_opts {
  int udp_port;              // -u  3000
  int ws_port;               // -p  3030
  int web_port;              // -w  80 (0 = off; forced off when built with WEBUI=0)
  const char *files_dir;     // -d  /media/emmc
  const char *usb_dir;       // -U  /media/usb
  const char *mounts_file;   // -M  /proc/mounts
  const char *hook_sock;     // -s  /tmp/gkhook.sock
  const char *var_dir;       // -v  /customer/gk/var
  const char *iface;         // -i  wlan0
  const char *mainboard_id;  // -m  override (tests), NULL = derive from iface
  int allow_tmpfs;           // -T  allow a tmpfs files dir (tests)
  int stale_seconds;         // -S  60  drop an upload session with no chunk for this long
  long long reserve_bytes;   // -R  free space kept back on uploads and USB copies (default FILES_RESERVE_BYTES)
};

struct conn;
uint32_t conn_id(const struct conn *c);
struct conn *conn_by_id(uint32_t id);        // NULL once the connection is closed

// Queue a text frame on a WebSocket connection (ignored for other connections).
void conn_ws_text(struct conn *c, const char *s, size_t n);
void ws_broadcast_text(const char *s, size_t n);

// Queue a complete HTTP/1.1 response and close the connection once it is sent.
void conn_http_reply(struct conn *c, int status, const char *ctype, const char *body, size_t n);

void loop_every(int ms, void (*fn)(void));
void loop_add_fd(int fd, void (*on_readable)(int fd));
// One idle callback, run once per loop iteration after the timers. It returns the milliseconds until it wants
// to run again (0 = right away, the loop then polls without waiting; -1 = nothing to do).
void loop_on_idle(int (*fn)(void));

// Hooks, set by later modules before loop_run. All run on the loop thread.
// A text message (not NUL-terminated); the text "ping" never reaches it.
extern void (*on_ws_text)(struct conn *, const char *s, size_t n);
// A connection has just become a WebSocket (the 101 reply is queued); it may send to it.
extern void (*on_ws_open)(struct conn *);
// Head of an Origin-checked POST on /uploadFile/upload (a foreign Origin gets 403 with the spec failure
// body, and HEAD always gets 200 with no body, both before the hook). Return > 0 to stream the body
// (clen bytes) into on_upload_data; return <= 0 when the hook has already replied. With no hook set,
// such requests get 404.
extern int (*on_upload_head)(struct conn *, const struct http_head *);
// Body bytes in order; (NULL, 0) marks the end of the body, after which the hook must
// reply. Return non-zero to abort (reply first; the connection closes after it is sent).
extern int (*on_upload_data)(struct conn *, const char *, size_t);
// Called once for every connection just before it is freed, with the connection already marked
// closed: conn_by_id(its id) is NULL. The hook may call conn_by_id, conn_ws_text, ws_broadcast_text
// and conn_id on any connection (all are still valid), but must not free anything. It must not
// keep the pointer afterwards.
extern void (*on_conn_close)(struct conn *);

// Listens on o->ws_port and runs until SIGTERM/SIGINT. Returns the process exit status.
int loop_run(const struct gk_opts *o);
#endif
