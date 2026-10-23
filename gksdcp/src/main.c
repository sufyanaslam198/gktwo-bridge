// gksdcp entry point: option parsing, then the event loop.
#include "discovery.h"
#include "files.h"
#include "history.h"
#include "hookw.h"
#include "ident.h"
#include "loop.h"
#include "sdcp.h"
#include "status.h"
#include "upload.h"
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

static void usage(const char *argv0) {
  fprintf(stderr,
          "usage: %s [-u udp_port] [-p ws_port] [-w web_port] [-d files_dir] [-U usb_dir]\n"
          "          [-M mounts_file] [-s hook_sock] [-v var_dir] [-i iface] [-m mainboard_id] [-S stale_seconds] [-R reserve_bytes] [-T]\n",
          argv0);
}

static int parse_port(const char *s, int *out) {
  char *end;
  long v = strtol(s, &end, 10);
  if (end == s || *end || v < 0 || v > 65535) return -1;
  *out = (int)v;
  return 0;
}

int main(int argc, char **argv) {
  struct gk_opts o = {
    .udp_port = 3000, .ws_port = 3030, .web_port = 80,
    .files_dir = "/media/emmc", .usb_dir = "/media/usb", .mounts_file = "/proc/mounts",
    .hook_sock = "/tmp/gkhook.sock", .var_dir = "/customer/gk/var", .iface = "wlan0",
    .mainboard_id = NULL, .allow_tmpfs = 0, .stale_seconds = 60,
    .reserve_bytes = FILES_RESERVE_BYTES,
  };
  int c;
  while ((c = getopt(argc, argv, "u:p:w:d:U:M:s:v:i:m:S:R:T")) != -1) {
    switch (c) {
      case 'u': if (parse_port(optarg, &o.udp_port) < 0) { usage(argv[0]); return 2; } break;
      case 'p': if (parse_port(optarg, &o.ws_port) < 0) { usage(argv[0]); return 2; } break;
      case 'w': if (parse_port(optarg, &o.web_port) < 0) { usage(argv[0]); return 2; } break;
      case 'd': o.files_dir = optarg; break;
      case 'U': o.usb_dir = optarg; break;
      case 'M': o.mounts_file = optarg; break;
      case 's': o.hook_sock = optarg; break;
      case 'v': o.var_dir = optarg; break;
      case 'i': o.iface = optarg; break;
      case 'm': o.mainboard_id = optarg; break;
      case 'S': {
        char *end;
        long v = strtol(optarg, &end, 10);
        if (end == optarg || *end || v < 1 || v > 86400) { usage(argv[0]); return 2; }
        o.stale_seconds = (int)v;
        break;
      }
      case 'R': {
        char *end;
        long long v = strtoll(optarg, &end, 10);
        if (end == optarg || *end || v < 0) { usage(argv[0]); return 2; }
        o.reserve_bytes = v;
        break;
      }
      case 'T': o.allow_tmpfs = 1; break;
      default: usage(argv[0]); return 2;
    }
  }
  if (optind != argc) { usage(argv[0]); return 2; }
#if !WEBUI
  o.web_port = 0;
#endif
  if (ident_init(o.iface, o.mainboard_id, o.var_dir) < 0) {
    fprintf(stderr, "gksdcp: -m must be 16 hex digits\n");
    return 2;
  }
  if (discovery_open(o.udp_port) < 0) return 1;
  if (files_init(o.files_dir, o.usb_dir, o.mounts_file) < 0) {
    fprintf(stderr, "gksdcp: -d, -U or -M path too long\n");
    return 2;
  }
  files_set_reserve(o.reserve_bytes);
  status_init(o.files_dir, o.iface);
  if (hookw_init(o.hook_sock) < 0) {
    fprintf(stderr, "gksdcp: cannot start the hook worker\n");
    return 1;
  }
  char hist[300];
  snprintf(hist, sizeof hist, "%s/history.json", o.var_dir);
  if (history_init(hist, HISTORY_CAP) < 0) {
    fprintf(stderr, "gksdcp: -v path too long\n");
    return 2;
  }
  history_attach();           // before sdcp_init: task ids must be set before the status push
  // must precede sdcp_init: it installs on_conn_close, which sdcp_init chains to
  upload_init(o.files_dir, o.stale_seconds, o.allow_tmpfs);
  sdcp_init();
  return loop_run(&o);
}
