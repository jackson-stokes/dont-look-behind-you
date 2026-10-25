#include "check.h"
#include "log.h"

#include <fcntl.h>
#include <unistd.h>

#define SEC 1000000000LL

static int pipe_rd;
static char expect_prefix[64];

/* Everything logged since the last call, NUL-terminated. */
static size_t drain(char *buf, size_t cap)
{
	size_t n = 0;
	ssize_t r = -1;

	while (n < cap - 1 && (r = read(pipe_rd, buf + n, cap - 1 - n)) > 0)
		n += (size_t)r;
	CHECK(n < cap - 1);
	CHECK(r < 0 && (errno == EAGAIN || errno == EWOULDBLOCK));
	buf[n] = '\0';
	return n;
}

static void test_line_format(void)
{
	char out[1024], want[256];

	log_msg(LOG_W, "degraded lost=%s", "radar");
	log_msg(LOG_I, "store_back dropped=%d", 37);
	drain(out, sizeof(out));
	snprintf(want, sizeof(want), "%s W degraded lost=radar\n%s I store_back dropped=37\n",
		 expect_prefix, expect_prefix);
	CHECK(strcmp(out, want) == 0);
}

static void test_rate_limit(void)
{
	struct log_limit lim = { 0 };
	char out[1024], want[512];
	int i;

	for (i = 0; i < 5; i++)
		log_limited(&lim, i * SEC, LOG_W, "sonar_lost reason=%s", "no_echo");
	drain(out, sizeof(out));
	snprintf(want, sizeof(want), "%s W sonar_lost reason=no_echo count=1\n", expect_prefix);
	CHECK(strcmp(out, want) == 0);

	/* 10 s after the last printed line: 4 suppressed + this one. */
	log_limited(&lim, 10 * SEC, LOG_W, "sonar_lost reason=%s", "no_echo");
	drain(out, sizeof(out));
	snprintf(want, sizeof(want), "%s W sonar_lost reason=no_echo count=5\n", expect_prefix);
	CHECK(strcmp(out, want) == 0);

	log_limited(&lim, 20 * SEC - 1, LOG_W, "sonar_lost reason=%s", "no_echo");
	CHECK(drain(out, sizeof(out)) == 0);
	log_limited(&lim, 20 * SEC, LOG_W, "sonar_lost reason=%s", "no_echo");
	drain(out, sizeof(out));
	snprintf(want, sizeof(want), "%s W sonar_lost reason=no_echo count=2\n", expect_prefix);
	CHECK(strcmp(out, want) == 0);
}

static void test_long_line_truncated(void)
{
	char big[1000], out[2048];
	size_t n;

	memset(big, 'x', sizeof(big) - 1);
	big[sizeof(big) - 1] = '\0';
	log_msg(LOG_E, "huge v=%s", big);
	n = drain(out, sizeof(out));
	CHECK(n == LOG_LINE_MAX);
	CHECK(strncmp(out, expect_prefix, strlen(expect_prefix)) == 0);
	CHECK(memcmp(out + n - 4, "...\n", 4) == 0);
}

static void test_errno_preserved(void)
{
	char out[256];

	errno = ENOENT;
	log_msg(LOG_I, "probe");
	CHECK(errno == ENOENT);
	drain(out, sizeof(out));
}

int main(void)
{
	int p[2], saved;

	CHECK(pipe2(p, O_CLOEXEC) == 0);
	CHECK(fcntl(p[0], F_SETFL, O_NONBLOCK) == 0);
	pipe_rd = p[0];
	saved = dup(STDERR_FILENO);
	CHECK(saved >= 0);
	CHECK(dup2(p[1], STDERR_FILENO) == STDERR_FILENO);

	log_init("fuse-hub");
	snprintf(expect_prefix, sizeof(expect_prefix), "fuse-hub[%ld]", (long)getpid());

	RUN(test_line_format);
	RUN(test_rate_limit);
	RUN(test_long_line_truncated);
	RUN(test_errno_preserved);

	CHECK(dup2(saved, STDERR_FILENO) == STDERR_FILENO);
	CHECK(close(saved) == 0 && close(p[0]) == 0 && close(p[1]) == 0);
	return 0;
}
