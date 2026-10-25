#include "ipc.h"
#include "msg.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/uio.h>
#include <sys/un.h>
#include <unistd.h>

#define BACKOFF_MIN_MS 50
#define BACKOFF_MAX_MS 1000
#define LISTEN_BACKLOG 8

static_assert(IPC_PATH_MAX == sizeof(((struct sockaddr_un *)0)->sun_path), "IPC_PATH_MAX");

const char *ipc_dir(void)
{
	const char *d = getenv(IPC_DIR_ENV);

	return (d && *d) ? d : IPC_DEFAULT_DIR;
}

int ipc_path(const char *name, char *buf, size_t cap)
{
	int n = snprintf(buf, cap, "%s/%s.sock", ipc_dir(), name);

	if (n < 0)
		return -1;
	if ((size_t)n >= cap) {
		errno = ENAMETOOLONG;
		return -1;
	}
	return 0;
}

static int fill_addr(struct sockaddr_un *sa, const char *path)
{
	size_t len = strlen(path);

	if (len >= sizeof(sa->sun_path)) {
		errno = ENAMETOOLONG;
		return -1;
	}
	memset(sa, 0, sizeof(*sa));
	sa->sun_family = AF_UNIX;
	memcpy(sa->sun_path, path, len + 1);
	return 0;
}

/* close() always releases the fd on Linux, so its error has nothing to retry. */
static void close_keep_errno(int fd)
{
	int err = errno;

	(void)close(fd);
	errno = err;
}

int ipc_listen(const char *name)
{
	const char *dir = ipc_dir();
	char path[IPC_PATH_MAX];
	struct sockaddr_un sa;
	struct stat st;
	int fd;

	if (mkdir(dir, 0700) < 0) {
		if (errno != EEXIST || stat(dir, &st) < 0)
			return -1;
		if (!S_ISDIR(st.st_mode)) {
			errno = ENOTDIR;
			return -1;
		}
	}
	if (ipc_path(name, path, sizeof(path)) < 0 || fill_addr(&sa, path) < 0)
		return -1;

	/* A socket left by a dead receiver is replaced; anything else is not ours. */
	if (lstat(path, &st) == 0) {
		if (!S_ISSOCK(st.st_mode)) {
			errno = EEXIST;
			return -1;
		}
		if (unlink(path) < 0)
			return -1;
	} else if (errno != ENOENT) {
		return -1;
	}

	fd = socket(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0);
	if (fd < 0)
		return -1;
	if (bind(fd, (struct sockaddr *)&sa, sizeof(sa)) < 0)
		goto fail;
	if (listen(fd, LISTEN_BACKLOG) < 0)
		goto fail;
	return fd;

fail:
	close_keep_errno(fd);
	return -1;
}

int ipc_accept(int lfd)
{
	return accept4(lfd, NULL, NULL, SOCK_CLOEXEC);
}

int ipc_conn_init(struct ipc_conn *c, const char *name)
{
	memset(c, 0, sizeof(*c));
	c->fd = -1;
	c->backoff_ms = BACKOFF_MIN_MS;
	return ipc_path(name, c->path, sizeof(c->path));
}

void ipc_conn_close(struct ipc_conn *c)
{
	if (c->fd >= 0) {
		close_keep_errno(c->fd);
		c->fd = -1;
	}
}

static int try_connect(struct ipc_conn *c, int64_t now_ns)
{
	struct sockaddr_un sa;
	int fd;

	if (fill_addr(&sa, c->path) < 0)
		goto backoff;
	/* Non-blocking so a full backlog fails with EAGAIN instead of stalling. */
	fd = socket(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC | SOCK_NONBLOCK, 0);
	if (fd < 0)
		goto backoff;
	if (connect(fd, (struct sockaddr *)&sa, sizeof(sa)) < 0) {
		close_keep_errno(fd);
		goto backoff;
	}
	c->fd = fd;
	c->backoff_ms = BACKOFF_MIN_MS;
	return 0;

backoff:
	c->next_try_ns = now_ns + (int64_t)c->backoff_ms * 1000000;
	c->backoff_ms = c->backoff_ms * 2 > BACKOFF_MAX_MS ? BACKOFF_MAX_MS : c->backoff_ms * 2;
	return -1;
}

int ipc_send(struct ipc_conn *c, const void *buf, size_t len, int64_t now_ns)
{
	ssize_t n;

	if (len == 0 || len > MSG_MAX) {
		errno = EINVAL;
		goto drop;
	}
	if (c->fd < 0) {
		if (now_ns < c->next_try_ns) {
			errno = ENOTCONN;
			goto drop;
		}
		if (try_connect(c, now_ns) < 0)
			goto drop;
	}

	n = send(c->fd, buf, len, MSG_NOSIGNAL | MSG_DONTWAIT);
	if (n == (ssize_t)len)
		return 0;
	if (n >= 0) {
		errno = EMSGSIZE;	/* seqpacket sends are atomic; not expected */
		goto drop;
	}
	if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR)
		goto drop;

	/* EPIPE, ECONNRESET, ...: the receiver is gone. Retry on the next send. */
	ipc_conn_close(c);
	c->next_try_ns = now_ns;
drop:
	c->dropped++;
	return -1;
}

ssize_t ipc_recv(int fd, void *buf, size_t cap, uint64_t *rejected)
{
	struct iovec iov = { .iov_base = buf, .iov_len = cap };
	struct msghdr mh = { .msg_iov = &iov, .msg_iovlen = 1 };
	ssize_t n;

	if (cap < MSG_MAX) {
		errno = EINVAL;
		return -1;
	}
	n = recvmsg(fd, &mh, 0);
	if (n < 0)
		return -1;
	if (mh.msg_flags & MSG_TRUNC) {
		(*rejected)++;
		errno = EMSGSIZE;
		return -1;
	}
	return n;
}

int ipc_check_msg(const void *buf, size_t len, uint64_t *rejected)
{
	uint8_t type;

	if (len < 1)
		goto bad;
	type = *(const uint8_t *)buf;
	if (msg_size(type) == 0 || len != msg_size(type))
		goto bad;
	return type;

bad:
	(*rejected)++;
	errno = EBADMSG;
	return -1;
}
