// Command queue + shared state between the socket threads and the UI tick.
// All shared state lives here behind one mutex.
#ifndef GKHOOK_QUEUE_H
#define GKHOOK_QUEUE_H
#include <stddef.h>
#include <stdint.h>

enum gk_cmd_kind { CMD_STATUS, CMD_START, CMD_PAUSE, CMD_RESUME, CMD_STOP,
                  CMD_Z_MOVE, CMD_Z_TOP, CMD_Z_HOME, CMD_Z_STOP };

struct gk_cmd {
  enum gk_cmd_kind kind;
  uint64_t id;       // set by the queue; 0 = nobody waits for a result
  char path[1100];   // CMD_START only
  uint32_t magic;    // CMD_START only: validated CTB magic read by the checker thread
  uint32_t gen;      // CMD_START only: the start generation it belongs to (gkq_start_gen)
  int mm;            // CMD_Z_MOVE only: relative step in mm, one of +-1 +-10 +-100 (positive = up)
};

// Tick-published snapshot. Unknown numeric fields are -1 (JSON null).
struct gk_status {
  char state[16];    // idle printing paused complete error
  int have_file;
  char file[1100];
  long layer, total_layers;
  double progress;   // < 0 unknown
  long elapsed_s, remaining_s;
  char message[128]; // last start failure, "" if none (filled by gkq_read_status)
  int z_avail;       // 0 => "z":null (Z control unavailable)
  int z_homed, z_moving;
  double z_pos;      // mm; serialized only when z_homed
  char phase[12];    // homing exposing pausing stopping, "" => "phase":null
  int screen_x, screen_y;  // panel resolution in pixels, 0 => "screen":null
  int print_window_open;  // internal, not serialized: window 16 with a printer (a finished print may be on screen)
};

// --- tick side ---
int  gkq_take(struct gk_cmd* out);                       // 1 if a command was dequeued
void gkq_set_result(uint64_t id, int ok, const char* err);  // late/unknown ids are ignored
void gkq_publish_status(const struct gk_status* st);
void gkq_end_start(const char* msg);                     // clears in-flight; msg NULL = success

// --- server side ---
// Posts and waits up to timeout_ms for the tick's result. Returns 1 ok, 0 failed (err filled).
int  gkq_post_wait(const struct gk_cmd* c, int timeout_ms, char* err, size_t errn);
int  gkq_post(const struct gk_cmd* c);                   // no wait; 1 queued, 0 queue full
// Returns 0 if the tick never published; else 1 and fills st. While a start is
// in flight st->state reads "starting".
int  gkq_read_status(struct gk_status* st);
uint32_t gkq_begin_start(const char* path);             // new start generation (> 0); 0 if one already in flight
int  gkq_end_start_gen(uint32_t gen, const char* msg);   // like gkq_end_start, but a no-op unless gen is in flight; 1 if it ended
void gkq_checker_done(uint32_t gen);                     // the checker for gen finished (no-op for a stale gen)
int  gkq_flight(uint32_t* gen, int* checker_done);       // 1 + fills both if a start is in flight
void gkq_clear_message(void);                            // drop a lingering start-failure message
int  gkq_start_in_flight(void);
uint32_t gkq_start_gen(void);                            // generation of the latest gkq_begin_start
int  gkq_start_valid(uint32_t gen);                      // 1 iff that start is still in flight
void gkq_reset(void);                                    // tests only
#endif
