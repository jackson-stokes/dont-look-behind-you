#include "hb.h"
#include "msg.h"

#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdlib.h>
#include <string.h>
#include <sys/prctl.h>
#include <sys/socket.h>
#include <unistd.h>

static int parse_long(const char *s, long lo, long hi, long *out)
{
	char *end;
	long v;

	errno = 0;
	v = strtol(s, &end, 10);
	if (errno || end == s || *end != '\0' || v < lo || v > hi) {
		errno = EINVAL;
		return -1;
	}
	*out = v;
	return 0;
}

int hb_init(struct hb *hb)
{
	const char *fd_s = getenv(HB_FD_ENV), *pid_s = getenv(HB_SUPER_PID_ENV);
	long fd, pid;

	memset(hb, 0, sizeof(*hb));
	hb->fd = -1;
	if (!fd_s || !*fd_s)
		return 0;
	if (parse_long(fd_s, 0, 65535, &fd) < 0)
		return -1;
	/* Inherited without CLOEXEC on purpose; don't pass it any further. */
	if (fcntl((int)fd, F_SETFD, FD_CLOEXEC) < 0)
		return -1;
	if (prctl(PR_SET_PDEATHSIG, SIGTERM) < 0)
		return -1;
	/* If super died before the prctl, we were reparented and no signal comes. */
	if (pid_s && *pid_s) {
		if (parse_long(pid_s, 1, 0x7fffffff, &pid) < 0)
			return -1;
		if (getppid() != (pid_t)pid) {
			errno = ESRCH;
			return -1;
		}
	}
	hb->fd = (int)fd;
	return 1;
}

int hb_tick(struct hb *hb, int64_t now_ns)
{
	struct hb_msg m;
	ssize_t n;

	if (hb->fd < 0 || now_ns < hb->next_ns)
		return 0;
	memset(&m, 0, sizeof(m));
	m.type = MSG_HB;
	m.seq = hb->seq++;
	hb->next_ns = now_ns + HB_PERIOD_NS;
	n = send(hb->fd, &m, sizeof(m), MSG_NOSIGNAL | MSG_DONTWAIT);
	if (n == (ssize_t)sizeof(m))
		return 0;
	if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR))
		return 0;
	if (n >= 0)
		errno = EMSGSIZE;
	return -1;
}

int hb_readable(struct hb *hb)
{
	char buf[16];
	ssize_t n;

	if (hb->fd < 0)
		return 0;
	n = recv(hb->fd, buf, sizeof(buf), MSG_DONTWAIT);
	if (n > 0)
		return 0;	/* super sends nothing today; ignore */
	if (n == 0) {
		errno = ENOTCONN;
		return -1;
	}
	if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR)
		return 0;
	return -1;
}

int64_t hb_due_ns(const struct hb *hb)
{
	return hb->fd < 0 ? INT64_MAX : hb->next_ns;
}
