#include "discovery.h"
#include "ident.h"
#include "json.h"
#include "loop.h"
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#ifndef MACHINE_NAME
#define MACHINE_NAME "GKtwo"
#endif
#ifndef BRAND_NAME
#define BRAND_NAME "Uniformation"
#endif

static int is_probe(const char *b, size_t n) {
  static const char want[] = "M99999";
  if (n < sizeof want - 1 || memcmp(b, want, sizeof want - 1) != 0) return 0;
  for (size_t i = sizeof want - 1; i < n; i++)
    if (b[i] != '\r' && b[i] != '\n') return 0;
  return 1;
}

static void on_readable(int fd) {
  char buf[64];
  union { struct cmsghdr h; char b[CMSG_SPACE(sizeof(struct in_pktinfo))]; } ctl;
  struct sockaddr_in from;
  struct iovec iov = { buf, sizeof buf };
  struct msghdr m = { .msg_name = &from, .msg_namelen = sizeof from, .msg_iov = &iov, .msg_iovlen = 1,
                      .msg_control = ctl.b, .msg_controllen = sizeof ctl.b };
  ssize_t n = recvmsg(fd, &m, 0);
  if (n < 0 || (m.msg_flags & (MSG_TRUNC | MSG_CTRUNC)) || !is_probe(buf, (size_t)n)) return;

  // The local address the probe arrived on. For a broadcast probe ipi_addr is the broadcast
  // address, so use ipi_spec_dst, which is always one of our own addresses.
  struct in_pktinfo pi;
  int have = 0;
  for (struct cmsghdr *c = CMSG_FIRSTHDR(&m); c; c = CMSG_NXTHDR(&m, c))
    if (c->cmsg_level == IPPROTO_IP && c->cmsg_type == IP_PKTINFO) {
      memcpy(&pi, CMSG_DATA(c), sizeof pi);
      have = 1;
    }
  if (!have || pi.ipi_spec_dst.s_addr == htonl(INADDR_ANY) || pi.ipi_spec_dst.s_addr == htonl(INADDR_BROADCAST)) return;
  char ip[16];
  if (ident_ip_for(&pi.ipi_spec_dst, ip) < 0) return;

  char out[768];
  struct jw w;
  jw_init(&w, out, sizeof out);
  jw_raw(&w, "{\"Id\":"); jw_str(&w, GK_BRAND_ID);
  jw_raw(&w, ",\"Data\":{\"Name\":"); jw_str(&w, ident_name());
  jw_raw(&w, ",\"MachineName\":"); jw_str(&w, MACHINE_NAME);
  jw_raw(&w, ",\"BrandName\":"); jw_str(&w, BRAND_NAME);
  jw_raw(&w, ",\"MainboardIP\":"); jw_str(&w, ip);
  jw_raw(&w, ",\"MainboardID\":"); jw_str(&w, ident_mainboard_id());
  jw_raw(&w, ",\"ProtocolVersion\":\"V3.0.0\",\"FirmwareVersion\":"); jw_str(&w, ident_fw_version());
  jw_raw(&w, "}}");
  if (w.err) return;

  // Reply from the address the probe was sent to, to the sender's address and port.
  struct in_pktinfo src = { .ipi_spec_dst = pi.ipi_spec_dst };
  union { struct cmsghdr h; char b[CMSG_SPACE(sizeof src)]; } sctl;
  memset(&sctl, 0, sizeof sctl);
  struct iovec riov = { out, w.len };
  struct msghdr r = { .msg_name = &from, .msg_namelen = sizeof from, .msg_iov = &riov, .msg_iovlen = 1,
                      .msg_control = sctl.b, .msg_controllen = sizeof sctl.b };
  struct cmsghdr *c = CMSG_FIRSTHDR(&r);
  c->cmsg_level = IPPROTO_IP;
  c->cmsg_type = IP_PKTINFO;
  c->cmsg_len = CMSG_LEN(sizeof src);
  memcpy(CMSG_DATA(c), &src, sizeof src);
  (void)sendmsg(fd, &r, MSG_DONTWAIT);     // best effort: UDP, the client retries
}

int discovery_open(int port) {
  int fd = socket(AF_INET, SOCK_DGRAM | SOCK_CLOEXEC | SOCK_NONBLOCK, 0);
  if (fd < 0) return -1;
  int one = 1;
  struct sockaddr_in a = { .sin_family = AF_INET, .sin_port = htons((uint16_t)port) };
  a.sin_addr.s_addr = htonl(INADDR_ANY);
  if (setsockopt(fd, IPPROTO_IP, IP_PKTINFO, &one, sizeof one) < 0 ||
      bind(fd, (struct sockaddr *)&a, sizeof a) < 0) {
    fprintf(stderr, "gksdcp: cannot bind udp port %d: %s\n", port, strerror(errno));
    close(fd);
    return -1;
  }
  loop_add_fd(fd, on_readable);
  return 0;
}
