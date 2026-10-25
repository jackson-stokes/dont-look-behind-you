#include "check.h"
#include "notify.h"

#include <stddef.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

static char base[] = "/tmp/dlby-notify-XXXXXX";

static void expect(int fd, const char *msg)
{
	char buf[64];
	ssize_t n = recv(fd, buf, sizeof(buf) - 1, MSG_DONTWAIT);

	CHECK(n == (ssize_t)strlen(msg));
	buf[n] = '\0';
	CHECK(strcmp(buf, msg) == 0);
}

static void test_unset_is_noop(void)
{
	CHECK(unsetenv(NOTIFY_SOCKET_ENV) == 0);
	CHECK(notify_send("WATCHDOG=1") == 0);
}

static void test_path_socket(void)
{
	struct sockaddr_un sa;
	char path[64];
	int fd;

	snprintf(path, sizeof(path), "%s/notify", base);
	memset(&sa, 0, sizeof(sa));
	sa.sun_family = AF_UNIX;
	memcpy(sa.sun_path, path, strlen(path) + 1);
	fd = socket(AF_UNIX, SOCK_DGRAM | SOCK_CLOEXEC, 0);
	CHECK(fd >= 0 && bind(fd, (struct sockaddr *)&sa, sizeof(sa)) == 0);
	CHECK(setenv(NOTIFY_SOCKET_ENV, path, 1) == 0);

	CHECK(notify_send("READY=1") == 0);
	expect(fd, "READY=1");
	CHECK(notify_send("WATCHDOG=1") == 0);
	expect(fd, "WATCHDOG=1");
	CHECK(close(fd) == 0 && unlink(path) == 0);
}

static void test_abstract_socket(void)
{
	struct sockaddr_un sa;
	char name[64];
	socklen_t len;
	int fd;

	snprintf(name, sizeof(name), "@dlby-notify-test-%ld", (long)getpid());
	memset(&sa, 0, sizeof(sa));
	sa.sun_family = AF_UNIX;
	memcpy(sa.sun_path, name, strlen(name));
	sa.sun_path[0] = '\0';
	len = (socklen_t)(offsetof(struct sockaddr_un, sun_path) + strlen(name));
	fd = socket(AF_UNIX, SOCK_DGRAM | SOCK_CLOEXEC, 0);
	CHECK(fd >= 0 && bind(fd, (struct sockaddr *)&sa, len) == 0);
	CHECK(setenv(NOTIFY_SOCKET_ENV, name, 1) == 0);

	CHECK(notify_send("WATCHDOG=1") == 0);
	expect(fd, "WATCHDOG=1");
	CHECK(close(fd) == 0);
}

static void test_bad_address(void)
{
	CHECK(setenv(NOTIFY_SOCKET_ENV, "relative/path", 1) == 0);
	errno = 0;
	CHECK(notify_send("WATCHDOG=1") == -1 && errno == EAFNOSUPPORT);
	CHECK(unsetenv(NOTIFY_SOCKET_ENV) == 0);
}

int main(void)
{
	CHECK(mkdtemp(base) != NULL);
	RUN(test_unset_is_noop);
	RUN(test_path_socket);
	RUN(test_abstract_socket);
	RUN(test_bad_address);
	CHECK(rmdir(base) == 0);
	return 0;
}
