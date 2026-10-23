#include "hook_io.h"

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <time.h>
#include <unistd.h>

static long long now_ms(void)
{
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (long long)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

/* wait for events until deadline; 1 ready, 0 timeout/error */
static int wait_fd(int fd, short ev, long long deadline)
{
	for (;;) {
		struct pollfd p = { fd, ev, 0 };
		long long left = deadline - now_ms();
		int r;
		if (left <= 0) return 0;
		r = poll(&p, 1, (int)left);
		if (r < 0 && errno == EINTR) continue;
		return r > 0;
	}
}

static int send_all(int fd, const char *b, size_t n, long long deadline)
{
	while (n) {
		ssize_t w = send(fd, b, n, MSG_NOSIGNAL);
		if (w < 0 && errno == EINTR) continue;
		if (w < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
			if (!wait_fd(fd, POLLOUT, deadline)) return -1;
			continue;
		}
		if (w <= 0) return -1;
		b += w;
		n -= (size_t)w;
	}
	return 0;
}

int hook_request(const char *sock_path, const char *json_line, char *reply,
                 size_t reply_len, int timeout_ms)
{
	struct sockaddr_un sa;
	long long deadline = now_ms() + timeout_ms;
	size_t used = 0, n;
	int fd, rc = -1;

	if (reply_len < 2 || strlen(sock_path) >= sizeof(sa.sun_path)) return -1;
	memset(&sa, 0, sizeof(sa));
	sa.sun_family = AF_UNIX;
	strcpy(sa.sun_path, sock_path);

	fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
	if (fd < 0) return -1;
	fcntl(fd, F_SETFL, fcntl(fd, F_GETFL) | O_NONBLOCK);

	/* AF_UNIX connect never blocks: it succeeds, fails, or EAGAIN when the backlog is full */
	while (connect(fd, (struct sockaddr *)&sa, sizeof(sa)) < 0) {
		if (errno == EINTR) continue;
		if (errno != EAGAIN || now_ms() >= deadline) goto out;
		usleep(10000);
	}

	n = strlen(json_line);
	if (send_all(fd, json_line, n, deadline) < 0) goto out;
	if ((n == 0 || json_line[n - 1] != '\n') && send_all(fd, "\n", 1, deadline) < 0) goto out;

	/* read the reply until its newline; EOF without one, NUL bytes, or overflow = failure */
	for (;;) {
		ssize_t r;
		char *e;
		if (!wait_fd(fd, POLLIN, deadline)) goto out;
		r = recv(fd, reply + used, reply_len - 1 - used, 0);
		if (r < 0 && (errno == EINTR || errno == EAGAIN)) continue;
		if (r < 0) goto out;
		if (r == 0) goto out; /* EOF before the terminating newline: truncated reply */
		if (memchr(reply + used, 0, (size_t)r)) goto out; /* NUL byte: not a JSON line */
		used += (size_t)r;
		reply[used] = 0;
		e = memchr(reply, '\n', used);
		if (e) { *e = 0; used = (size_t)(e - reply); break; }
		if (used >= reply_len - 1) goto out; /* reply too large */
	}
	while (used && reply[used - 1] == '\r') reply[--used] = 0;
	rc = used ? 0 : -1;
out:
	close(fd);
	return rc;
}
