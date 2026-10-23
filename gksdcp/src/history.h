// Print tasks and history: one record per print (TaskId, times, status), persisted to a small bounded file,
// answered as SDCP Cmd 320 (id list) and 321 (details).
#ifndef GKSDCP_HISTORY_H
#define GKSDCP_HISTORY_H
#include "hookw.h"
#include "json.h"

#define HISTORY_CAP 50

// Loads path (parent directories are created when a write needs them). At most cap entries (<= HISTORY_CAP)
// are kept, the oldest dropped. A missing file starts empty; a corrupt one is renamed to <path>.bad and
// history starts empty. 0 ok / -1 bad arguments.
int history_init(const char *path, int cap);

// New UUID v4 (36 chars, lowercase) into out.
void history_new_id(char out[37]);

// Opens a task now (EndTime 0, TaskStatus 0), closing a stale open one (no print behind it) as stopped.
// Also sets g_bridge.task_id and g_bridge.client_file (NULL or "" for a print the bridge did not start).
// Returns the TaskId (valid until the next call), or NULL and no change while an open task belongs to a
// live print (state starting/printing/paused): the caller must refuse the start.
// Call it BEFORE queuing the hook's start, so touchscreen detection does not open a second task.
const char *history_begin(const char *task_name, const char *client_file);

// The hook refused the start: deletes (not ends) the task history_begin just opened, restores
// g_bridge.task_id and g_bridge.client_file to their earlier values and saves. No-op if there is no such
// open task or the hook has already reported its print.
void history_abort(void);

// A live print (touchscreen) started while the open task was waiting for the bridge's own start: the task now
// belongs to that print. Takes its name and file from h and forgets the bridge's pending begin.
void history_adopt(const struct hk_status *h);

// Closes the open task: EndTime = clock_now(). task_status 1 completed, 2 error, 3 stopped; reason 0 or 19.
// No open task: no-op.
void history_end(int task_status, long layers, int reason);

// The start that history_begin opened ended without a print: closes that task as stopped (3), with no layers.
// No-op when the open task is another one (for example a touchscreen print) or there is none.
void history_end_begun(void);

// Call on every snapshot change, before the status push. Opens a task when a print starts with none open
// (touchscreen prints) and closes it when the print ends: complete 1, error 2 (reason 19), idle 3.
void history_track(const struct hk_status *h);

// Start-up: an open record is resumed if h is printing the same file, else closed as stopped.
// Also opens a task for a print that is already running with none recorded.
void history_recover(const struct hk_status *h);

// Recovers from the current snapshot, subscribes history_track to snapshot changes and registers
// Cmd 320 and 321. Call after history_init and hookw_init, before sdcp_init and loop_run.
void history_attach(void);

// Writers: TaskIds newest first as a JSON array; details of the records named in the array at ids_tok
// of d (unknown ids skipped, duplicates once, not an array -> []).
void history_ids(struct jw *w);
void history_details(struct jw *w, const struct jdoc *d, int ids_tok);
#endif
