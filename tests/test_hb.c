#include "check.h"
#include "hb.h"
#include "msg.h"

#include <fcntl.h>
#include <sys/socket.h>
#include <unistd.h>

#define SEC 1000000000LL

static void set_env_fd(int fd)
{
	char s[16];

	snprintf(s, sizeof(s), "%d", fd);
	CHECK(setenv(HB_FD_ENV, s, 1) == 0);
}

static void set_env_pid(long pid)
{
	char s[24];

	snprintf(s, sizeof(s), "%ld", pid);
	CHECK(setenv(HB_SUPER_PID_ENV, s, 1) == 0);
}

/* Next heartbeat on fd, or -1 if none is waiting. */
static int recv_hb(int fd, struct hb_msg *m)
{
	uint8_t buf[MSG_MAX];
	ssize_t n = recv(fd, buf, sizeof(buf), MSG_DONTWAIT);

	if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK))
		return -1;
	CHECK(n == (ssize_t)sizeof(*m));
	memcpy(m, buf, sizeof(*m));
	CHECK(m->type == MSG_HB && m->pad[0] == 0 && m->pad[1] == 0 && m->pad[2] == 0);
	return 0;
}

static void test_standalone(void)
{
	struct hb hb;

	CHECK(unsetenv(HB_FD_ENV) == 0 && unsetenv(HB_SUPER_PID_ENV) == 0);
	CHECK(hb_init(&hb) == 0);
	CHECK(hb.fd == -1 && hb_due_ns(&hb) == INT64_MAX);
	CHECK(hb_tick(&hb, 0) == 0 && hb_readable(&hb) == 0);
}

static void test_ticks_once_per_second(void)
{
	struct hb_msg m;
	struct hb hb;
	int sv[2], fl;

	CHECK(socketpair(AF_UNIX, SOCK_SEQPACKET, 0, sv) == 0);	/* no CLOEXEC, as inherited */
	set_env_fd(sv[1]);
	set_env_pid((long)getppid());
	CHECK(hb_init(&hb) == 1 && hb.fd == sv[1]);
	fl = fcntl(sv[1], F_GETFD);
	CHECK(fl >= 0 && (fl & FD_CLOEXEC));

	CHECK(hb_tick(&hb, 0) == 0);
	CHECK(recv_hb(sv[0], &m) == 0 && m.seq == 0);
	CHECK(hb_due_ns(&hb) == SEC);
	CHECK(hb_tick(&hb, SEC / 2) == 0 && recv_hb(sv[0], &m) == -1);
	CHECK(hb_tick(&hb, SEC) == 0 && recv_hb(sv[0], &m) == 0 && m.seq == 1);
	CHECK(hb_tick(&hb, 2 * SEC - 1) == 0 && recv_hb(sv[0], &m) == -1);
	CHECK(hb_tick(&hb, 2 * SEC) == 0 && recv_hb(sv[0], &m) == 0 && m.seq == 2);

	/* super gone: EOF on read, EPIPE on send */
	CHECK(hb_readable(&hb) == 0);
	CHECK(close(sv[0]) == 0);
	CHECK(hb_readable(&hb) == -1);
	CHECK(hb_tick(&hb, 3 * SEC) == -1 && errno == EPIPE);
	CHECK(close(sv[1]) == 0);
}

static void test_wrong_parent_and_bad_fd(void)
{
	struct hb hb;
	int sv[2];

	CHECK(socketpair(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0, sv) == 0);
	set_env_fd(sv[1]);
	set_env_pid(2147483646L);			/* not our parent */
	errno = 0;
	CHECK(hb_init(&hb) == -1 && errno == ESRCH && hb.fd == -1);

	CHECK(setenv(HB_FD_ENV, "abc", 1) == 0);
	errno = 0;
	CHECK(hb_init(&hb) == -1 && errno == EINVAL);
	CHECK(close(sv[0]) == 0 && close(sv[1]) == 0);
	CHECK(unsetenv(HB_FD_ENV) == 0 && unsetenv(HB_SUPER_PID_ENV) == 0);
}

int main(void)
{
	RUN(test_standalone);
	RUN(test_ticks_once_per_second);
	RUN(test_wrong_parent_and_bad_fd);
	return 0;
}
