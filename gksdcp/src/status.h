// gkhook snapshot + bridge state -> SDCP CurrentStatus / PrintInfo.Status, status and attributes frames.
#ifndef GKSDCP_STATUS_H
#define GKSDCP_STATUS_H
#include "hookw.h"
#include "json.h"

struct bridge_state {
  int uploading;
  int usb_copying;
  char task_id[37];
  char client_file[600];
  int sub_status;            // last PrintInfo.Status while idle or uploading
  int prev_status;
  int error_number;
  char message[160];
};
extern struct bridge_state g_bridge;

int status_current(const struct hk_status *, const struct bridge_state *);
int status_sub(const struct hk_status *, const struct bridge_state *);

// Whole push frames, including MainboardID, TimeStamp and Topic. Check w->err.
void status_write(struct jw *, const struct hk_status *, const struct bridge_state *);
void attributes_write(struct jw *, const struct hk_status *);

// Files dir (for RemainingMemory) and network interface (for MainboardIP).
// Call on every snapshot or bridge-state change (before pushing): maintains g_bridge.prev_status
// (the CurrentStatus before the latest change) and latches g_bridge.sub_status when a print or
// start ends (complete 9, error 8, back to idle 8).
void status_track(const struct hk_status *);

void status_init(const char *files_dir, const char *iface);
const char *status_files_dir(void);      // as given to status_init
// 1 when the address of the interface (MainboardIP) is not the one of the last call; the first call compares
// with "".
int status_ip_changed(void);
#endif
