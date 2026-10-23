#include "status.h"
#include "clock.h"
#include "files.h"
#include "ident.h"
#include <net/if.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <sys/statvfs.h>
#include <unistd.h>

#ifndef MACHINE_NAME
#define MACHINE_NAME "GKtwo"
#endif
#ifndef BRAND_NAME
#define BRAND_NAME "Uniformation"
#endif
#ifndef XYZ_SIZE
#define XYZ_SIZE "228.1x128.3x245"
#endif

struct bridge_state g_bridge;

static char g_files_dir[256];
static char g_iface[IFNAMSIZ];

void status_init(const char *files_dir, const char *iface) {
  snprintf(g_files_dir, sizeof g_files_dir, "%s", files_dir ? files_dir : "");
  snprintf(g_iface, sizeof g_iface, "%s", iface ? iface : "");
}

const char *status_files_dir(void) { return g_files_dir; }

// ---- state tables ----

static int is(const struct hk_status *h, const char *state) { return strcmp(h->state, state) == 0; }

// Fills cur and sub per the design's table (first match wins).
static void classify(const struct hk_status *h, const struct bridge_state *b, int *cur, int *sub) {
  *cur = 0;
  *sub = b->sub_status;
  if (b->usb_copying || is(h, "starting")) { *cur = 1; *sub = 10; }
  else if (is(h, "printing")) {
    *cur = 1;
    if (strcmp(h->phase, "homing") == 0) *sub = 1;
    else if (strcmp(h->phase, "pausing") == 0) *sub = 5;
    else if (strcmp(h->phase, "stopping") == 0) *sub = 7;
    else *sub = 3;
  }
  else if (is(h, "paused")) { *cur = 1; *sub = 6; }
  else if (is(h, "complete")) { *cur = 0; *sub = 9; }
  else if (is(h, "error")) { *cur = 0; *sub = 8; }
  else if (b->uploading) *cur = 2;
}

int status_current(const struct hk_status *h, const struct bridge_state *b) {
  int c, s;
  classify(h, b, &c, &s);
  return c;
}

int status_sub(const struct hk_status *h, const struct bridge_state *b) {
  int c, s;
  classify(h, b, &c, &s);
  return s;
}

// ---- change tracking ----

static int track_cur;                       // CurrentStatus at the last call
static char track_state[16] = "idle";

static int is_running_state(const char *s) {
  return strcmp(s, "starting") == 0 || strcmp(s, "printing") == 0 || strcmp(s, "paused") == 0;
}

void status_track(const struct hk_status *h) {
  int cur = status_current(h, &g_bridge);
  if (cur != track_cur) {
    g_bridge.prev_status = track_cur;
    track_cur = cur;
  }
  if (strcmp(h->state, track_state) != 0) {
    if (is(h, "complete")) g_bridge.sub_status = 9;
    else if (is(h, "error")) g_bridge.sub_status = 8;
    else if (is(h, "idle") && is_running_state(track_state)) g_bridge.sub_status = 8;   // stopped
    snprintf(track_state, sizeof track_state, "%s", h->state);
  }
}

// ---- frames ----

// Closes the frame's first member object, then adds MainboardID, TimeStamp and Topic.
static void tail(struct jw *w, const char *kind) {
  char topic[64];
  const char *id = ident_mainboard_id();
  jw_raw(w, "},\"MainboardID\":"); jw_str(w, id);
  jw_raw(w, ",\"TimeStamp\":"); jw_int(w, clock_now());
  snprintf(topic, sizeof topic, "sdcp/%s/%s", kind, id);
  jw_raw(w, ",\"Topic\":"); jw_str(w, topic);
  jw_raw(w, "}");
}

void status_write(struct jw *w, const struct hk_status *h, const struct bridge_state *b) {
  int cur, sub;
  classify(h, b, &cur, &sub);
  int running = is_running_state(h->state) || b->usb_copying;

  jw_raw(w, "{\"Status\":{\"CurrentStatus\":["); jw_int(w, cur);
  jw_raw(w, "],\"PreviousStatus\":"); jw_int(w, b->prev_status);
  jw_raw(w, ",\"PrintScreen\":0,\"ReleaseFilm\":0,\"TempOfUVLED\":0,\"TimeLapseStatus\":0,"
            "\"TempOfBox\":0,\"TempTargetBox\":0,\"PrintInfo\":{\"Status\":");
  jw_int(w, sub);
  jw_raw(w, ",\"CurrentLayer\":"); jw_int(w, h->layer > 0 ? h->layer : 0);
  jw_raw(w, ",\"TotalLayer\":"); jw_int(w, h->total_layers > 0 ? h->total_layers : 0);
  jw_raw(w, ",\"CurrentTicks\":"); jw_int(w, h->elapsed_s > 0 ? (long long)h->elapsed_s * 1000 : 0);
  jw_raw(w, ",\"TotalTicks\":");
  jw_int(w, (h->elapsed_s >= 0 && h->remaining_s >= 0) ? ((long long)h->elapsed_s + h->remaining_s) * 1000 : 0);

  char name[sizeof h->file + 8];
  name[0] = 0;
  if (b->client_file[0]) snprintf(name, sizeof name, "%s", b->client_file);
  else if (h->file[0]) {
    const char *base = strrchr(h->file, '/');
    snprintf(name, sizeof name, "/local/%s", base ? base + 1 : h->file);
  }
  jw_raw(w, ",\"Filename\":"); jw_str(w, name);
  jw_raw(w, ",\"ErrorNumber\":"); jw_int(w, b->error_number);
  jw_raw(w, ",\"TaskId\":"); jw_str(w, running ? b->task_id : "");

  jw_raw(w, "},\"GKBridge\":{\"HookOk\":"); jw_bool(w, h->hook_ok);
  jw_raw(w, ",\"Z\":");
  if (h->z_avail) {
    jw_raw(w, "{\"Pos\":");
    if (h->z_homed) jw_dbl(w, h->z_pos, 2); else jw_raw(w, "null");
    jw_raw(w, ",\"Homed\":"); jw_bool(w, h->z_homed);
    jw_raw(w, ",\"Moving\":"); jw_bool(w, h->z_moving);
    jw_raw(w, "}");
  } else {
    jw_raw(w, "null");
  }
  char msg[sizeof h->hook_error + 8];
  const char *m = "";
  if (h->message[0]) m = h->message;
  else if (b->message[0]) m = b->message;
  else if (!h->hook_ok) { snprintf(msg, sizeof msg, "hook: %s", h->hook_error); m = msg; }
  jw_raw(w, ",\"Message\":"); jw_str(w, m);
  jw_raw(w, "}");                          // GKBridge; tail() closes Status
  tail(w, "status");
}

static void iface_ip(char out[16]) {
  out[0] = 0;
  int fd = socket(AF_INET, SOCK_DGRAM | SOCK_CLOEXEC, 0);
  if (fd < 0) return;
  struct ifreq ifr;
  memset(&ifr, 0, sizeof ifr);
  snprintf(ifr.ifr_name, sizeof ifr.ifr_name, "%s", g_iface);
  if (g_iface[0] && ioctl(fd, SIOCGIFADDR, &ifr) == 0) {
    struct sockaddr_in a;
    memcpy(&a, &ifr.ifr_addr, sizeof a);
    if (ident_ip_for(&a.sin_addr, out) < 0) out[0] = 0;
  }
  close(fd);
}

int status_ip_changed(void) {
  static char last[16];
  char ip[16];
  iface_ip(ip);
  if (strcmp(ip, last) == 0) return 0;
  memcpy(last, ip, sizeof last);
  return 1;
}

static unsigned long long free_bytes(void) {
  struct statvfs sv;
  if (!g_files_dir[0] || statvfs(g_files_dir, &sv) < 0) return 0;
  return (unsigned long long)sv.f_bavail * sv.f_frsize;
}

void attributes_write(struct jw *w, const struct hk_status *h) {
  char res[24], ip[16];
  if (h->screen_x > 0 && h->screen_y > 0) snprintf(res, sizeof res, "%dx%d", h->screen_x, h->screen_y);
  else snprintf(res, sizeof res, "7680x4320");
  iface_ip(ip);

  jw_raw(w, "{\"Attributes\":{\"Name\":"); jw_str(w, ident_name());
  jw_raw(w, ",\"MachineName\":"); jw_str(w, MACHINE_NAME);
  jw_raw(w, ",\"BrandName\":"); jw_str(w, BRAND_NAME);
  jw_raw(w, ",\"ProtocolVersion\":\"V3.0.0\",\"FirmwareVersion\":"); jw_str(w, ident_fw_version());
  jw_raw(w, ",\"Resolution\":"); jw_str(w, res);
  jw_raw(w, ",\"XYZsize\":"); jw_str(w, XYZ_SIZE);
  jw_raw(w, ",\"MainboardIP\":"); jw_str(w, ip);
  jw_raw(w, ",\"MainboardID\":"); jw_str(w, ident_mainboard_id());
  jw_raw(w, ",\"NumberOfVideoStreamConnected\":0,\"MaximumVideoStreamAllowed\":0,\"NetworkStatus\":\"wlan\","
            "\"UsbDiskStatus\":");
  jw_int(w, usb_mounted() ? 1 : 0);
  jw_raw(w, ",\"Capabilities\":[\"FILE_TRANSFER\",\"PRINT_CONTROL\"],\"SupportFileType\":[\"CTB\"],"
            "\"DevicesStatus\":{\"TempSensorStatusOfUVLED\":0,\"LCDStatus\":1,\"SgStatus\":0,\"ZMotorStatus\":");
  jw_int(w, h->z_avail ? 1 : 0);
  jw_raw(w, ",\"RotateMotorStatus\":0,\"RelaseFilmState\":0,\"XMotorStatus\":0},"
            "\"ReleaseFilmMax\":0,\"TempOfUVLEDMax\":0,\"CameraStatus\":0,\"RemainingMemory\":");
  jw_int(w, (long long)free_bytes());
  jw_raw(w, ",\"TLPNoCapPos\":0,\"TLPStartCapPos\":0,\"TLPInterLayers\":0");
  tail(w, "attributes");
}
