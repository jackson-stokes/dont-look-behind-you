#include "check.h"
#include "ipc.h"
#include "msg.h"

#include <dirent.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#define MS 1000000LL

/* Sockets live under a fresh /tmp dir, never under /run or the bind-mounted tree. */
static char base[] = "/tmp/dlby-test-XXXXXX";
static char run[64];

/* Every message is zeroed first, as msg.h requires. */
static void fill_alert(struct alert_msg *m, uint32_t seq)
{
	memset(m, 0, sizeof(*m));
	m->type = MSG_ALERT;
	m->state = ST_CLEAR;
	m->seq = seq;
	m->origin_ns = 1000 + seq;
}

static void fill_sonar(struct sonar_msg *m, uint32_t seq)
{
	memset(m, 0, sizeof(*m));
	m->type = MSG_SONAR;
	m->seq = seq;
	m->echo_ns = 5800000;
}

static void fill_radar(struct radar_msg *m, uint32_t seq)
{
	memset(m, 0, sizeof(*m));
	m->type = MSG_RADAR;
	m->seq = seq;
	m->ntargets = 1;
	m->t[0].range_cm = 2500;
	m->t[0].speed_cms = -400;
}

static void fill_hb(struct hb_msg *m, uint32_t seq)
{
	memset(m, 0, sizeof(*m));
	m->type = MSG_HB;
	m->seq = seq;
}

/* Accept with a timeout so a broken sender fails the test instead of hanging it. */
static int accept_within(int lfd, int timeout_ms)
{
	struct pollfd p = { .fd = lfd, .events = POLLIN };

	if (poll(&p, 1, timeout_ms) != 1)
		return -1;
	return ipc_accept(lfd);
}

static bool is_cloexec(int fd)
{
	int fl = fcntl(fd, F_GETFD);

	return fl >= 0 && (fl & FD_CLOEXEC);
}

static void test_listen_connect_accept(void)
{
	struct ipc_conn c;
	struct alert_msg m;
	struct stat st;
	uint8_t buf[MSG_MAX];
	uint64_t rej = 0;
	int lfd, afd;

	lfd = ipc_listen("t1");
	CHECK(lfd >= 0);
	CHECK(stat(run, &st) == 0 && (st.st_mode & 0777) == 0700);
	CHECK(ipc_conn_init(&c, "t1") == 0);
	fill_alert(&m, 1);
	CHECK(ipc_send(&c, &m, sizeof(m), 0) == 0);
	afd = accept_within(lfd, 2000);
	CHECK(afd >= 0);
	CHECK(ipc_recv(afd, buf, sizeof(buf), &rej) == (ssize_t)sizeof(m));
	CHECK(memcmp(buf, &m, sizeof(m)) == 0);
	CHECK(rej == 0 && c.dropped == 0);
	ipc_conn_close(&c);
	CHECK(close(afd) == 0 && close(lfd) == 0);
}

static void test_message_boundaries(void)
{
	struct ipc_conn c;
	struct sonar_msg s;
	struct radar_msg r;
	struct hb_msg h;
	uint8_t buf[MSG_MAX];
	uint64_t rej = 0;
	int lfd, afd;

	lfd = ipc_listen("t2");
	CHECK(lfd >= 0);
	CHECK(ipc_conn_init(&c, "t2") == 0);
	fill_sonar(&s, 1);
	fill_radar(&r, 2);
	fill_hb(&h, 3);
	CHECK(ipc_send(&c, &s, sizeof(s), 0) == 0);
	CHECK(ipc_send(&c, &r, sizeof(r), 0) == 0);
	CHECK(ipc_send(&c, &h, sizeof(h), 0) == 0);
	errno = 0;
	CHECK(ipc_send(&c, &h, 0, 0) == -1 && errno == EINVAL);
	afd = accept_within(lfd, 2000);
	CHECK(afd >= 0);

	CHECK(ipc_recv(afd, buf, sizeof(buf), &rej) == 24);
	CHECK(ipc_check_msg(buf, 24, &rej) == MSG_SONAR && memcmp(buf, &s, 24) == 0);
	CHECK(ipc_recv(afd, buf, sizeof(buf), &rej) == 64);
	CHECK(ipc_check_msg(buf, 64, &rej) == MSG_RADAR && memcmp(buf, &r, 64) == 0);
	CHECK(ipc_recv(afd, buf, sizeof(buf), &rej) == 8);
	CHECK(ipc_check_msg(buf, 8, &rej) == MSG_HB && memcmp(buf, &h, 8) == 0);
	CHECK(rej == 0);
	ipc_conn_close(&c);
	CHECK(close(afd) == 0 && close(lfd) == 0);
}

static void test_peer_death_is_eof(void)
{
	struct alert_msg m;
	uint8_t buf[MSG_MAX];
	uint64_t rej = 0;
	int lfd, afd, status;
	pid_t pid;

	lfd = ipc_listen("t3");
	CHECK(lfd >= 0);
	pid = fork();
	CHECK(pid >= 0);
	if (pid == 0) {
		struct ipc_conn c;

		if (ipc_conn_init(&c, "t3") < 0)
			_exit(2);
		fill_alert(&m, 7);
		if (ipc_send(&c, &m, sizeof(m), 0) < 0)
			_exit(3);
		pause();
		_exit(0);
	}
	afd = accept_within(lfd, 2000);
	CHECK(afd >= 0);
	CHECK(ipc_recv(afd, buf, sizeof(buf), &rej) == (ssize_t)sizeof(m));
	CHECK(kill(pid, SIGKILL) == 0);
	CHECK(waitpid(pid, &status, 0) == pid && WIFSIGNALED(status));
	CHECK(ipc_recv(afd, buf, sizeof(buf), &rej) == 0);
	CHECK(close(afd) == 0 && close(lfd) == 0);
}

static void test_send_to_dead_peer_drops(void)
{
	struct ipc_conn c;
	struct alert_msg m;
	int lfd, afd;

	lfd = ipc_listen("t4");
	CHECK(lfd >= 0);
	CHECK(ipc_conn_init(&c, "t4") == 0);
	fill_alert(&m, 1);
	CHECK(ipc_send(&c, &m, sizeof(m), 0) == 0);
	afd = accept_within(lfd, 2000);
	CHECK(afd >= 0);
	CHECK(close(afd) == 0 && close(lfd) == 0);

	errno = 0;
	CHECK(ipc_send(&c, &m, sizeof(m), 1) == -1);
	CHECK(errno == EPIPE || errno == ECONNRESET);
	CHECK(c.fd == -1 && c.dropped == 1);
}

static void test_reconnect_with_backoff(void)
{
	static const int64_t delay_ms[] = { 50, 100, 200, 400, 800, 1000, 1000 };
	struct ipc_conn c;
	struct alert_msg m;
	uint8_t buf[MSG_MAX];
	uint64_t rej = 0;
	int64_t t = 0;
	size_t i;
	int lfd, afd;

	/* A receiver that died: its socket file is left behind, nobody listening. */
	lfd = ipc_listen("t5");
	CHECK(lfd >= 0);
	CHECK(close(lfd) == 0);

	CHECK(ipc_conn_init(&c, "t5") == 0);
	fill_alert(&m, 1);
	for (i = 0; i < sizeof(delay_ms) / sizeof(delay_ms[0]); i++) {
		errno = 0;
		CHECK(ipc_send(&c, &m, sizeof(m), t) == -1 && errno == ECONNREFUSED);
		CHECK(c.next_try_ns == t + delay_ms[i] * MS);
		errno = 0;
		CHECK(ipc_send(&c, &m, sizeof(m), c.next_try_ns - 1) == -1 && errno == ENOTCONN);
		t = c.next_try_ns;
	}

	/* The receiver restarts on the same path, replacing the stale socket. */
	lfd = ipc_listen("t5");
	CHECK(lfd >= 0);
	CHECK(ipc_send(&c, &m, sizeof(m), t) == 0);
	CHECK(c.backoff_ms == 50);
	CHECK(c.dropped == 2 * (sizeof(delay_ms) / sizeof(delay_ms[0])));
	afd = accept_within(lfd, 2000);
	CHECK(afd >= 0);
	CHECK(ipc_recv(afd, buf, sizeof(buf), &rej) == (ssize_t)sizeof(m));
	ipc_conn_close(&c);
	CHECK(close(afd) == 0 && close(lfd) == 0);
}

static void test_connect_missing_socket(void)
{
	struct ipc_conn c;
	struct alert_msg m;

	CHECK(ipc_conn_init(&c, "never-listened") == 0);
	fill_alert(&m, 1);
	errno = 0;
	CHECK(ipc_send(&c, &m, sizeof(m), 0) == -1 && errno == ENOENT);
	CHECK(c.next_try_ns == 50 * MS && c.dropped == 1);
}

static void test_listen_refuses_non_socket(void)
{
	char path[IPC_PATH_MAX];
	struct stat st;
	int fd;

	CHECK(ipc_path("t6", path, sizeof(path)) == 0);
	fd = open(path, O_CREAT | O_WRONLY | O_CLOEXEC, 0600);
	CHECK(fd >= 0);
	CHECK(close(fd) == 0);
	errno = 0;
	CHECK(ipc_listen("t6") == -1 && errno == EEXIST);
	CHECK(lstat(path, &st) == 0 && S_ISREG(st.st_mode));
}

static void test_fds_are_cloexec(void)
{
	struct ipc_conn c;
	struct alert_msg m;
	int lfd, afd;

	lfd = ipc_listen("t7");
	CHECK(lfd >= 0 && is_cloexec(lfd));
	CHECK(ipc_conn_init(&c, "t7") == 0);
	fill_alert(&m, 1);
	CHECK(ipc_send(&c, &m, sizeof(m), 0) == 0);
	CHECK(is_cloexec(c.fd));
	afd = accept_within(lfd, 2000);
	CHECK(afd >= 0 && is_cloexec(afd));
	ipc_conn_close(&c);
	CHECK(close(afd) == 0 && close(lfd) == 0);
}

static void test_recv_rejects_truncated(void)
{
	struct alert_msg m;
	uint8_t big[100], buf[MSG_MAX], small[32];
	uint64_t rej = 0;
	int sv[2];

	CHECK(socketpair(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0, sv) == 0);
	memset(big, 0, sizeof(big));
	big[0] = MSG_RADAR;
	fill_alert(&m, 1);
	CHECK(send(sv[0], big, sizeof(big), MSG_NOSIGNAL) == (ssize_t)sizeof(big));
	CHECK(send(sv[0], &m, sizeof(m), MSG_NOSIGNAL) == (ssize_t)sizeof(m));

	errno = 0;
	CHECK(ipc_recv(sv[1], buf, sizeof(buf), &rej) == -1 && errno == EMSGSIZE);
	CHECK(rej == 1);
	/* The oversized message was consumed; the next one arrives intact. */
	CHECK(ipc_recv(sv[1], buf, sizeof(buf), &rej) == (ssize_t)sizeof(m));
	CHECK(memcmp(buf, &m, sizeof(m)) == 0);
	errno = 0;
	CHECK(ipc_recv(sv[1], small, sizeof(small), &rej) == -1 && errno == EINVAL);
	CHECK(rej == 1);
	CHECK(close(sv[0]) == 0 && close(sv[1]) == 0);
}

static void test_check_msg_length_vs_type(void)
{
	static const uint8_t types[] = { MSG_RADAR, MSG_SONAR, MSG_ALERT, MSG_HB };
	uint8_t b[MSG_MAX];
	uint64_t rej = 0;
	size_t i;

	memset(b, 0, sizeof(b));
	b[0] = MSG_RADAR;
	errno = 0;
	CHECK(ipc_check_msg(b, 24, &rej) == -1 && errno == EBADMSG);
	b[0] = 99;
	errno = 0;
	CHECK(ipc_check_msg(b, 16, &rej) == -1 && errno == EBADMSG);
	b[0] = MSG_SONAR;
	errno = 0;
	CHECK(ipc_check_msg(b, 4, &rej) == -1 && errno == EBADMSG);
	CHECK(ipc_check_msg(b, 0, &rej) == -1);
	CHECK(rej == 4);

	for (i = 0; i < sizeof(types); i++) {
		b[0] = types[i];
		CHECK(ipc_check_msg(b, msg_size(types[i]), &rej) == types[i]);
	}
	CHECK(msg_size(MSG_RADAR) == 64 && msg_size(MSG_SONAR) == 24);
	CHECK(msg_size(MSG_ALERT) == 16 && msg_size(MSG_HB) == 8);
	CHECK(rej == 4);
}

static void cleanup(void)
{
	char path[128];
	struct dirent *de;
	DIR *d = opendir(run);

	if (d) {
		while ((de = readdir(d)) != NULL) {
			if (de->d_name[0] == '.')
				continue;
			snprintf(path, sizeof(path), "%s/%.40s", run, de->d_name);
			CHECK(unlink(path) == 0);
		}
		CHECK(closedir(d) == 0);
		CHECK(rmdir(run) == 0);
	}
	CHECK(rmdir(base) == 0);
}

int main(void)
{
	CHECK(mkdtemp(base) != NULL);
	snprintf(run, sizeof(run), "%s/run", base);
	CHECK(setenv(IPC_DIR_ENV, run, 1) == 0);

	RUN(test_listen_connect_accept);	/* creates run/ */
	RUN(test_message_boundaries);
	RUN(test_peer_death_is_eof);
	RUN(test_send_to_dead_peer_drops);
	RUN(test_reconnect_with_backoff);
	RUN(test_connect_missing_socket);
	RUN(test_listen_refuses_non_socket);
	RUN(test_fds_are_cloexec);
	RUN(test_recv_rejects_truncated);
	RUN(test_check_msg_length_vs_type);

	cleanup();
	return 0;
}
