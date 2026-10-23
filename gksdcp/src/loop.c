#include "loop.h"
#include "ws.h"
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

#if WEBUI
#include "index_html.h"        // xxd -i of www/index.html: www_index_html, www_index_html_len
#endif

#define MAX_FDS    8
#define MAX_TIMERS 8
#define WS_RX_MAX  (MAX_WS_PAYLOAD + 14)      // largest masked frame
#define WS_RX_INIT 2048
#define BODY_CHUNK 8192
#define TX_BUDGET  (4 * 1024 * 1024)          // pending send bytes across all connections
#define TX_HIGH    (64 * 1024)                // stop reading from a connection above this
#define TX_LOW     (16 * 1024)                // ... and resume below this
#define MAX_REFUSED 8                         // over-limit sockets kept briefly to deliver their 503
#define REFUSED_MS  2000
#define ACCEPT_PAUSE_MS 100

enum { ST_HEAD, ST_BODY, ST_BODY_DONE, ST_WS };

struct conn {
  int fd;
  uint32_t id;
  int state;
  int dead;            // closed at the end of the current loop pass
  int close_after;     // close once the send queue drains; input is read and discarded meanwhile
  int shut;            // write side shut down (close_after reply fully sent)
  int throttled;       // not reading: send queue is backed up
  int notified;        // on_conn_close has run
  int web;             // accepted on the web port: serves the page only
  unsigned char *rx;
  size_t rxn, rxcap;
  unsigned char *tx;
  size_t txoff, txn, txcap;
  long long t_start, t_last;
  long long body_left;
  struct http_head head;
};

void (*on_ws_text)(struct conn *, const char *, size_t);
void (*on_ws_open)(struct conn *);
int (*on_upload_head)(struct conn *, const struct http_head *);
int (*on_upload_data)(struct conn *, const char *, size_t);
void (*on_conn_close)(struct conn *);

static struct conn *conns[MAX_CONNS];
static int nconns;
static uint32_t next_id = 1;
static volatile sig_atomic_t g_stop;

static size_t tx_total;                       // pending send bytes, all connections
static struct { int fd; long long until; } refused[MAX_REFUSED];
static int n_refused;
static long long accept_pause_until;
static char sink[BODY_CHUNK];                 // discard buffer

static struct { int fd; void (*fn)(int); } extra_fds[MAX_FDS];
static int n_extra;
static struct { int ms; void (*fn)(void); long long next; } timers[MAX_TIMERS];
static int n_timers;

static long long now_ms(void) {
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return (long long)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

uint32_t conn_id(const struct conn *c) { return c->id; }

struct conn *conn_by_id(uint32_t id) {
  for (int i = 0; i < nconns; i++)
    if (conns[i]->id == id && !conns[i]->dead) return conns[i];
  return NULL;
}

void loop_every(int ms, void (*fn)(void)) {
  if (n_timers == MAX_TIMERS) return;
  timers[n_timers].ms = ms;
  timers[n_timers].fn = fn;
  timers[n_timers].next = now_ms() + ms;
  n_timers++;
}

static int (*idle_fn)(void);
static long long idle_wait = -1;
void loop_on_idle(int (*fn)(void)) { idle_fn = fn; }

void loop_add_fd(int fd, void (*on_readable)(int fd)) {
  if (n_extra == MAX_FDS) return;
  extra_fds[n_extra].fd = fd;
  extra_fds[n_extra].fn = on_readable;
  n_extra++;
}

// ---- send queue ----

static void tx_append(struct conn *c, const void *p, size_t n) {
  if (c->dead) return;
  size_t pending = c->txn - c->txoff;
  if (pending + n > SEND_QUEUE_CAP) {
    fprintf(stderr, "gksdcp: conn %u: send queue over %d bytes, closing\n", c->id, SEND_QUEUE_CAP);
    c->dead = 1;
    return;
  }
  if (tx_total + n > TX_BUDGET) {
    fprintf(stderr, "gksdcp: conn %u: total send budget of %d bytes exhausted, closing\n", c->id, TX_BUDGET);
    c->dead = 1;
    return;
  }
  if (c->txn + n > c->txcap) {
    if (c->txoff) {                      // reclaim the sent prefix first
      memmove(c->tx, c->tx + c->txoff, pending);
      c->txn = pending;
      c->txoff = 0;
    }
    if (c->txn + n > c->txcap) {
      size_t cap = c->txcap ? c->txcap : 1024;
      while (cap < c->txn + n) cap *= 2;
      unsigned char *t = realloc(c->tx, cap);
      if (!t) { c->dead = 1; return; }
      c->tx = t;
      c->txcap = cap;
    }
  }
  memcpy(c->tx + c->txn, p, n);
  c->txn += n;
  tx_total += n;
}

static void set_close_after(struct conn *c) {
  c->close_after = 1;
  c->t_last = now_ms();
}

static void flush(struct conn *c) {
  while (!c->dead && c->txoff < c->txn) {
    ssize_t r = send(c->fd, c->tx + c->txoff, c->txn - c->txoff, MSG_NOSIGNAL);
    if (r > 0) { c->txoff += (size_t)r; tx_total -= (size_t)r; continue; }
    if (r < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) return;
    if (r < 0 && errno == EINTR) continue;
    c->dead = 1;
    return;
  }
  if (c->txoff == c->txn) {
    c->txoff = c->txn = 0;
    if (c->tx && c->txcap > 16384) { free(c->tx); c->tx = NULL; c->txcap = 0; }
    // reply fully sent: tell the peer we are done, then keep draining its input until it closes
    if (c->close_after && !c->shut) { shutdown(c->fd, SHUT_WR); c->shut = 1; }
  }
}

static void send_frame(struct conn *c, int op, const void *p, size_t n) {
  unsigned char h[10];
  tx_append(c, h, ws_header(h, op, n));
  if (n) tx_append(c, p, n);
}

void conn_ws_text(struct conn *c, const char *s, size_t n) {
  if (c->state == ST_WS && !c->close_after) send_frame(c, WS_OP_TEXT, s, n);
}

void ws_broadcast_text(const char *s, size_t n) {
  for (int i = 0; i < nconns; i++) conn_ws_text(conns[i], s, n);
}

// Close frame with a status code, then close once it is sent.
static void ws_fail(struct conn *c, unsigned code) {
  unsigned char p[2] = {(unsigned char)(code >> 8), (unsigned char)code};
  send_frame(c, WS_OP_CLOSE, p, 2);
  set_close_after(c);
}

// ---- HTTP replies ----

static const char *reason(int status) {
  switch (status) {
    case 200: return "OK";
    case 400: return "Bad Request";
    case 403: return "Forbidden";
    case 404: return "Not Found";
    case 409: return "Conflict";
    case 411: return "Length Required";
    case 413: return "Payload Too Large";
    case 500: return "Internal Server Error";
    case 503: return "Service Unavailable";
    case 507: return "Insufficient Storage";
    default:  return "Error";
  }
}

void conn_http_reply(struct conn *c, int status, const char *ctype, const char *body, size_t n) {
  // The page on the web port reads upload failures from this port: a browser only allows that when the reply
  // names the page's origin, so echo the request's Origin, and only when origin_allowed() accepts it.
  // X-Frame-Options and nosniff keep the page from being framed or reinterpreted.
  char extra[384] = "X-Frame-Options: DENY\r\nX-Content-Type-Options: nosniff\r\n";
  const struct http_head *rq = &c->head;
  if (rq->has_origin && rq->origin[0] && origin_allowed(rq))
    snprintf(extra + strlen(extra), sizeof extra - strlen(extra), "Access-Control-Allow-Origin: %s\r\nVary: Origin\r\n", rq->origin);
  char h[640];
  int hn = snprintf(h, sizeof h, "HTTP/1.1 %d %s\r\nContent-Type: %s\r\nContent-Length: %zu\r\n%sConnection: close\r\n\r\n",
                    status, reason(status), ctype ? ctype : "text/plain", n, extra);
  if (hn < 0 || (size_t)hn >= sizeof h)        // content type too long for the buffer: same headers, plain type
    hn = snprintf(h, sizeof h, "HTTP/1.1 %d %s\r\nContent-Type: text/plain\r\nContent-Length: %zu\r\n%sConnection: close\r\n\r\n",
                  status, reason(status), n, extra);
  tx_append(c, h, (size_t)hn);
  if (n) tx_append(c, body, n);
  set_close_after(c);
}

static void reply_status(struct conn *c, int status) {
  conn_http_reply(c, status, "text/plain", NULL, 0);
}

// ---- request routing ----

static void upload_end(struct conn *c) {
  c->state = ST_BODY_DONE;
  if (on_upload_data) on_upload_data(c, NULL, 0);
}

static void body_feed(struct conn *c, const char *p, size_t n) {
  if ((long long)n > c->body_left) n = (size_t)c->body_left;
  if (n && on_upload_data && on_upload_data(c, p, n) != 0) { set_close_after(c); return; }
  c->body_left -= (long long)n;
  if (c->body_left == 0) upload_end(c);
}

static void route_head(struct conn *c) {
  const struct http_head *h = &c->head;
  if (c->web) {
#if WEBUI
    if (strcmp(h->method, "GET") == 0 && strcmp(h->path, "/") == 0) {
      conn_http_reply(c, 200, "text/html; charset=utf-8", (const char *)www_index_html, www_index_html_len);
      return;
    }
#endif
    reply_status(c, 404);
    return;
  }
  if (strcmp(h->path, "/websocket") == 0) {
    if (strcmp(h->method, "GET") != 0 || !h->upgrade_ws || !h->ws_key[0]) { reply_status(c, 400); return; }
    if (!origin_allowed(h)) { reply_status(c, 403); return; }
    char key[29], resp[192];
    ws_accept_key(h->ws_key, key);
    int n = snprintf(resp, sizeof resp,
                     "HTTP/1.1 101 Switching Protocols\r\nUpgrade: websocket\r\nConnection: Upgrade\r\nSec-WebSocket-Accept: %s\r\n\r\n", key);
    tx_append(c, resp, (size_t)n);
    c->state = ST_WS;
    c->t_last = now_ms();
    if (on_ws_open) on_ws_open(c);
    return;
  }
  int is_post = strcmp(h->method, "POST") == 0;
  if (on_upload_head && strcmp(h->path, "/uploadFile/upload") == 0 && (is_post || strcmp(h->method, "HEAD") == 0)) {
    if (!is_post) { reply_status(c, 200); return; }          // reachability probe: any Origin, no body
    if (!origin_allowed(h)) {
      static const char refused[] = "{\"code\":\"111111\",\"messages\":[{\"field\":\"common_field\",\"message\":-4},"
                                    "{\"field\":\"common_field\",\"message\":\"origin not allowed\"}],\"data\":null,\"success\":false}";
      conn_http_reply(c, 403, "application/json", refused, sizeof refused - 1);
      return;
    }
    int r = on_upload_head(c, h);
    if (r > 0 && is_post) {
      c->state = ST_BODY;
      c->body_left = h->clen;
      c->t_last = now_ms();
      if (c->body_left == 0) upload_end(c);
    } else if (!c->close_after) {
      reply_status(c, 500);          // the hook refused without replying
    }
    return;
  }
  reply_status(c, 404);
}

// ---- WebSocket messages ----

static void ws_message(struct conn *c, const struct ws_frame *f) {
  switch (f->op) {
    case WS_OP_PING: send_frame(c, WS_OP_PONG, f->payload, f->len); break;
    case WS_OP_PONG: break;
    case WS_OP_CLOSE:
      if (f->len == 1) { ws_fail(c, 1002); break; }
      send_frame(c, WS_OP_CLOSE, f->payload, f->len >= 2 ? 2 : 0);
      set_close_after(c);
      break;
    case WS_OP_TEXT:
      if (!f->fin) { ws_fail(c, 1003); break; }
      if (f->len == 4 && memcmp(f->payload, "ping", 4) == 0) send_frame(c, WS_OP_TEXT, "pong", 4);
      else if (on_ws_text) on_ws_text(c, (const char *)f->payload, f->len);
      break;
    case WS_OP_BIN: if (!f->fin) ws_fail(c, 1003); break;     // binary frames are ignored
    default:        ws_fail(c, 1003); break;                  // includes continuation frames
  }
}

// ---- input ----

static void rx_consume(struct conn *c, size_t n) {
  memmove(c->rx, c->rx + n, c->rxn - n);
  c->rxn -= n;
}

static void process(struct conn *c) {
  while (!c->dead && !c->close_after && c->rxn) {
    if (c->state == ST_HEAD) {
      int r = http_parse_head((const char *)c->rx, c->rxn, &c->head);
      if (r == 0) return;
      if (r < 0) { reply_status(c, 400); return; }
      rx_consume(c, (size_t)r);
      route_head(c);
    } else if (c->state == ST_BODY) {
      size_t n = c->rxn;
      if ((long long)n > c->body_left) n = (size_t)c->body_left;
      body_feed(c, (const char *)c->rx, n);
      rx_consume(c, n);
    } else if (c->state == ST_WS) {
      struct ws_frame f;
      long r = ws_decode(c->rx, c->rxn, MAX_WS_PAYLOAD, &f);
      if (r == 0) return;
      if (r < 0) { ws_fail(c, 1002); return; }
      ws_message(c, &f);
      rx_consume(c, (size_t)r);
    } else {
      return;                         // ST_BODY_DONE: nothing more is expected
    }
  }
}

static int rx_room(struct conn *c) {
  size_t limit = c->state == ST_WS ? WS_RX_MAX : HTTP_MAX_HEAD;
  if (c->rxn == c->rxcap && c->rxcap < limit) {
    size_t cap = c->rxcap ? c->rxcap * 2 : WS_RX_INIT;
    if (cap > limit) cap = limit;
    unsigned char *p = realloc(c->rx, cap);
    if (!p) { c->dead = 1; return 0; }
    c->rx = p;
    c->rxcap = cap;
  }
  return c->rxcap > c->rxn;
}

// Input nobody wants (after a reply was queued, or after the body ended) is read and thrown
// away until the peer closes; closing with unread input would reset the connection and lose the reply.
static int discarding(const struct conn *c) { return c->close_after || c->state == ST_BODY_DONE; }

static void on_readable(struct conn *c) {
  ssize_t r;
  if (discarding(c)) {
    r = recv(c->fd, sink, sizeof sink, 0);
  } else if (c->state == ST_BODY) {
    char tmp[BODY_CHUNK];
    size_t want = sizeof tmp;
    if ((long long)want > c->body_left) want = (size_t)c->body_left;
    r = recv(c->fd, tmp, want, 0);
    if (r > 0) { c->t_last = now_ms(); body_feed(c, tmp, (size_t)r); }
  } else {
    if (!rx_room(c)) { c->dead = 1; return; }
    r = recv(c->fd, c->rx + c->rxn, c->rxcap - c->rxn, 0);
    if (r > 0) {
      c->rxn += (size_t)r;
      c->t_last = now_ms();
      process(c);
      if (c->rxn == 0 && c->rxcap > 16384) { free(c->rx); c->rx = NULL; c->rxcap = 0; }
    }
  }
  if (r == 0 || (r < 0 && errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR)) c->dead = 1;
}

// ---- connections ----

static void accept_all(int lfd, int web) {
  for (;;) {
    int fd = accept4(lfd, NULL, NULL, SOCK_NONBLOCK | SOCK_CLOEXEC);
    if (fd < 0) {
      if (errno == EMFILE || errno == ENFILE || errno == ENOBUFS || errno == ENOMEM)
        accept_pause_until = now_ms() + ACCEPT_PAUSE_MS;     // do not spin on a full descriptor table
      return;
    }
    if (nconns >= MAX_CONNS) {
      static const char busy[] = "HTTP/1.1 503 Service Unavailable\r\nContent-Length: 0\r\nConnection: close\r\n\r\n";
      (void)send(fd, busy, sizeof busy - 1, MSG_NOSIGNAL);
      shutdown(fd, SHUT_WR);
      if (n_refused < MAX_REFUSED) {           // linger so the client can read the 503
        refused[n_refused].fd = fd;
        refused[n_refused].until = now_ms() + REFUSED_MS;
        n_refused++;
      } else {
        close(fd);
      }
      continue;
    }
    int one = 1;
    setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one);
    struct conn *c = calloc(1, sizeof *c);
    if (!c) { close(fd); continue; }
    c->rx = malloc(HTTP_MAX_HEAD);
    if (!c->rx) { free(c); close(fd); continue; }
    c->rxcap = HTTP_MAX_HEAD;
    c->fd = fd;
    c->web = web;
    c->id = next_id++;
    c->state = ST_HEAD;
    c->t_start = c->t_last = now_ms();
    conns[nconns++] = c;
  }
}

static void check_deadline(struct conn *c, long long now) {
  long long limit;
  if (c->close_after) limit = HEAD_DEADLINE_MS;
  else if (c->state == ST_HEAD) { if (now - c->t_start > HEAD_DEADLINE_MS) c->dead = 1; return; }
  else if (c->state == ST_WS) limit = WS_IDLE_MS;
  else limit = UPLOAD_IDLE_MS;
  if (now - c->t_last > limit) c->dead = 1;
}

static void sweep(void) {
  // Notify every dead connection first, while conns[] is intact, so a hook may use conn_by_id /
  // ws_broadcast_text safely. A hook can mark more connections dead, hence the repeat.
  int again;
  do {
    again = 0;
    for (int i = 0; i < nconns; i++) {
      struct conn *c = conns[i];
      if (c->dead && !c->notified) {
        c->notified = 1;
        if (on_conn_close) on_conn_close(c);
        again = 1;
      }
    }
  } while (again);

  int j = 0;
  for (int i = 0; i < nconns; i++) {
    struct conn *c = conns[i];
    if (!c->dead) { conns[j++] = c; continue; }
    tx_total -= c->txn - c->txoff;
    shutdown(c->fd, SHUT_WR);
    close(c->fd);
    free(c->rx);
    free(c->tx);
    free(c);
  }
  nconns = j;
}

static int tcp_listen(int port) {
  int fd = socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
  if (fd < 0) return -1;
  int one = 1;
  setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
  struct sockaddr_in a = {0};
  a.sin_family = AF_INET;
  a.sin_addr.s_addr = htonl(INADDR_ANY);
  a.sin_port = htons((uint16_t)port);
  if (bind(fd, (struct sockaddr *)&a, sizeof a) < 0 || listen(fd, 16) < 0) { close(fd); return -1; }
  return fd;
}

// Drain and retire the over-limit sockets (pf[] entries start at first).
static void service_refused(const struct pollfd *pf, int nr, long long now) {
  for (int i = 0; i < nr; i++) {
    int done = now >= refused[i].until;
    if (pf[i].revents & (POLLIN | POLLHUP | POLLERR)) {
      ssize_t r = recv(refused[i].fd, sink, sizeof sink, 0);
      if (r == 0 || (r < 0 && errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR)) done = 1;
    }
    if (done) { close(refused[i].fd); refused[i].fd = -1; }
  }
  int j = 0;
  for (int i = 0; i < n_refused; i++)
    if (refused[i].fd >= 0) refused[j++] = refused[i];
  n_refused = j;
}

static void on_signal(int sig) { (void)sig; g_stop = 1; }

int loop_run(const struct gk_opts *o) {
  signal(SIGPIPE, SIG_IGN);
  struct sigaction sa = {0};
  sa.sa_handler = on_signal;               // no SA_RESTART: poll() returns EINTR
  sigaction(SIGTERM, &sa, NULL);
  sigaction(SIGINT, &sa, NULL);

  int lfd = tcp_listen(o->ws_port);
  if (lfd < 0) { fprintf(stderr, "gksdcp: cannot listen on tcp port %d: %s\n", o->ws_port, strerror(errno)); return 1; }

  int wfd = -1;                            // web port listener, -1 when off (poll ignores a negative fd)
#if WEBUI
  if (o->web_port > 0) {
    wfd = tcp_listen(o->web_port);
    if (wfd < 0) fprintf(stderr, "gksdcp: cannot listen on web port %d: %s (no web page)\n", o->web_port, strerror(errno));
  }
#endif

  struct pollfd pf[2 + MAX_CONNS + MAX_FDS + MAX_REFUSED];
  while (!g_stop) {
    int np = 0, ns = nconns;
    long long now = now_ms();
    int accepting = now >= accept_pause_until;
    pf[np].fd = lfd; pf[np].events = accepting ? POLLIN : 0; np++;
    pf[np].fd = wfd; pf[np].events = accepting ? POLLIN : 0; np++;
    for (int i = 0; i < ns; i++) {
      struct conn *c = conns[i];
      size_t pending = c->txn - c->txoff;       // backpressure: stop reading while the peer is not
      if (pending > TX_HIGH) c->throttled = 1;
      else if (pending < TX_LOW) c->throttled = 0;
      pf[np].fd = c->fd;
      pf[np].events = ((discarding(c) || !c->throttled) ? POLLIN : 0) | (pending ? POLLOUT : 0);
      np++;
    }
    for (int i = 0; i < n_extra; i++) { pf[np].fd = extra_fds[i].fd; pf[np].events = POLLIN; np++; }
    int nr = n_refused;
    for (int i = 0; i < nr; i++) { pf[np].fd = refused[i].fd; pf[np].events = POLLIN; np++; }

    long long wait = 1000;                 // deadlines are checked at least once a second
    if (!accepting && accept_pause_until - now < wait) wait = accept_pause_until - now;
    for (int i = 0; i < n_timers; i++) {
      long long d = timers[i].next - now;
      if (d < wait) wait = d < 0 ? 0 : d;
    }
    if (idle_wait >= 0 && idle_wait < wait) wait = idle_wait;
    int pr = poll(pf, (nfds_t)np, (int)wait);
    if (pr < 0 && errno != EINTR) { perror("gksdcp: poll"); break; }

    if (pr > 0) {
      for (int i = 0; i < ns; i++) {
        struct conn *c = conns[i];
        short ev = pf[2 + i].revents;
        if (c->dead || !ev) continue;
        if (ev & POLLIN) on_readable(c);
        else if (ev & (POLLHUP | POLLERR)) c->dead = 1;
        if (!c->dead && (ev & POLLOUT)) flush(c);
        if (!c->dead && (ev & POLLNVAL)) c->dead = 1;
      }
      if (pf[0].revents & POLLIN) accept_all(lfd, 0);
      if (pf[1].revents & POLLIN) accept_all(wfd, 1);
      for (int i = 0; i < n_extra; i++)
        if (pf[2 + ns + i].revents & (POLLIN | POLLHUP | POLLERR)) extra_fds[i].fn(extra_fds[i].fd);
    }
    if (pr >= 0) service_refused(pf + 2 + ns + n_extra, nr, now_ms());

    now = now_ms();
    for (int i = 0; i < n_timers; i++)
      if (now >= timers[i].next) { timers[i].next = now + timers[i].ms; timers[i].fn(); }
    idle_wait = idle_fn ? idle_fn() : -1;

    // push whatever the handlers queued, then retire dead connections
    for (int i = 0; i < nconns; i++) {
      flush(conns[i]);
      check_deadline(conns[i], now);
    }
    sweep();
  }
  for (int i = 0; i < nconns; i++) conns[i]->dead = 1;
  sweep();
  close(lfd);
  if (wfd >= 0) close(wfd);
  return 0;
}
