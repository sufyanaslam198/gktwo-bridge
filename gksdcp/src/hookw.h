// gkhook worker thread: polls status every 250 ms, runs queued commands, wakes the loop through a pipe.
#ifndef GKSDCP_HOOKW_H
#define GKSDCP_HOOKW_H

struct hk_status {
  int hook_ok;               // 0 when the hook is unreachable or answered ok:false
  char hook_error[96];
  char state[16];            // idle starting printing paused complete error
  char file[1100];
  long layer, total_layers;  // -1 = unknown, as gkhook's null
  long elapsed_s, remaining_s;
  char phase[12];            // homing exposing pausing stopping, or ""
  int screen_x, screen_y;    // -1 = unknown
  int z_avail, z_homed, z_moving;
  double z_pos;
  char message[128];
};

// Fill out from one status reply line. Never fails: a bad or ok:false reply gives hook_ok=0,
// the error text and state "idle".
void hookw_parse_reply(const char *reply, struct hk_status *out);

// Starts the thread (after one synchronous poll). Registers its wake-up pipe with loop_add_fd,
// so call it before loop_run. 0 ok / -1.
int hookw_init(const char *sock);
// Latest snapshot (a copy). Returns its poll number: every completed status poll counts, from 1.
unsigned long hookw_status(struct hk_status *out);

// Queue one JSON command line (max 16 queued, -1 when full or too long). cb runs on the loop thread.
// Timeout 2300 ms from the call: ok=0, err="printer not responding".
// ctx must carry a connection id (uint32_t), never a struct conn*: the connection can close before the
// callback runs. Answer through sdcp_reply(conn, ...), which is safe for a closed connection.
typedef void (*hookw_done)(void *ctx, int ok, const char *err);
int hookw_call(const char *json_line, hookw_done cb, void *ctx);
// Inside a hookw_done callback: the poll number when that command's reply came. A snapshot with a higher
// number comes from a poll made after the reply.
unsigned long hookw_call_seq(void);

// Subscribe fn to snapshot changes (loop thread). The poll after each command also counts as a change,
// whether or not the snapshot differs. Up to 8 listeners; -1 when full. Call before loop_run.
int hookw_on_change(void (*fn)(void));
#endif
