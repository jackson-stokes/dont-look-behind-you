#include "notify.h"

#include <errno.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

int notify_send(const char *msg)
{
	const char *path = getenv(NOTIFY_SOCKET_ENV);
	struct sockaddr_un sa;
	size_t plen, mlen = strlen(msg);
	socklen_t alen;
	ssize_t n;
	int fd, err;

	if (!path || !*path)
		return 0;
	plen = strlen(path);
	if (path[0] != '/' && path[0] != '@') {
		errno = EAFNOSUPPORT;
		return -1;
	}
	if (plen >= sizeof(sa.sun_path)) {
		errno = ENAMETOOLONG;
		return -1;
	}
	memset(&sa, 0, sizeof(sa));
	sa.sun_family = AF_UNIX;
	memcpy(sa.sun_path, path, plen);
	if (path[0] == '@')
		sa.sun_path[0] = '\0';	/* abstract namespace: exact length, no NUL */
	alen = (socklen_t)(offsetof(struct sockaddr_un, sun_path) + plen);

	fd = socket(AF_UNIX, SOCK_DGRAM | SOCK_CLOEXEC, 0);
	if (fd < 0)
		return -1;
	n = sendto(fd, msg, mlen, MSG_NOSIGNAL, (struct sockaddr *)&sa, alen);
	err = errno;
	(void)close(fd);	/* the fd is released even if close reports an error */
	if (n != (ssize_t)mlen) {
		errno = n < 0 ? err : EMSGSIZE;
		return -1;
	}
	return 0;
}
