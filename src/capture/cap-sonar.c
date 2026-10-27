/* cap-sonar: pings the HC-SR04 every 60 ms and sends one sonar_msg per answered
 * ping to fuse-hub (DESIGN.md §1, §2 C). Heartbeats to dlby-super from the main
 * loop (hb.h); standalone when not supervised.
 *
 *   $DLBY_SONAR_CHIP  GPIO chip (default /dev/gpiochip0: the Pi 5 header, RP1)
 *   $DLBY_SONAR_TRIG  TRIG line (default 23)
 *   $DLBY_SONAR_ECHO  ECHO line (default 24, through the divider)
 *
 * Pings run on absolute 60 ms deadlines (timerfd, TFD_TIMER_ABSTIME), so a late
 * wakeup doesn't push the later ones back. Range comes from the kernel's
 * timestamps of the ECHO edges (GPIO uAPI v2, CLOCK_MONOTONIC), not from when
 * this process runs. A ping is classified at the next deadline (echo.h); no
 * edge or a stuck-high line sends nothing, so fuse-hub's staleness timeout
 * marks the sonar lost.
 *
 * Exit codes: 0 on SIGTERM/SIGINT or when dlby-super is gone, 78 on a bad
 * setting or missing chip (restarting won't fix it), 69 if the lines are busy
 * or the GPIO calls fail, 70 on any other unexpected failure.
 */
#include "echo.h"
#include "hb.h"
#include "health.h"
#include "ipc.h"
#include "log.h"
#include "msg.h"

#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <linux/gpio.h>
#include <poll.h>
#include <signal.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/signalfd.h>
#include <sys/timerfd.h>
#include <sysexits.h>
#include <time.h>
#include <unistd.h>

#define CHIP_ENV     "DLBY_SONAR_CHIP"
#define TRIG_ENV     "DLBY_SONAR_TRIG"
#define ECHO_ENV     "DLBY_SONAR_ECHO"
#define CHIP_DEFAULT "/dev/gpiochip0"
#define TRIG_DEFAULT 23
#define ECHO_DEFAULT 24

#define TRIG_PULSE_NS 10000	/* HC-SR04 needs >= 10 us */
#define EDGE_BUF      16	/* kernel event buffer, DESIGN.md §2 C */
#define MAX_EDGES     16	/* edges kept per ping; a clean ping has 2 */
#define LOSS_PINGS    (int)(SONAR_LOSS_NS / SONAR_PERIOD_NS)
#define POLL_MS       100

struct sonar {
	int trig_fd, echo_fd;
	struct ipc_conn hub;
	struct echo_edge edge[MAX_EDGES];
	size_t nedge;
	uint64_t edges_dropped;
	int64_t trig_ns;	/* when the current ping's trigger went high */
	bool armed;		/* a ping is in flight */
	uint32_t seq, trigger_seq;
	uint64_t sent, far, missed, overruns;
	int miss_run;		/* unanswered pings in a row */
	bool up, lost;
	struct log_limit lim_send, lim_overrun, lim_edges;
};

static int parse_line(const char *env, int dflt, unsigned *out)
{
	const char *s = getenv(env);
	char *end;
	long v;

	if (!s || !*s) {
		*out = (unsigned)dflt;
		return 0;
	}
	errno = 0;
	v = strtol(s, &end, 10);
	if (errno || end == s || *end != '\0' || v < 0 || v > 511) {
		errno = EINVAL;
		return -1;
	}
	*out = (unsigned)v;
	return 0;
}

/* One line from chip as a request fd: output driven low, or input with both edges. */
static int request_line(int chip, unsigned offset, bool output)
{
	struct gpio_v2_line_request req;

	memset(&req, 0, sizeof(req));
	req.offsets[0] = offset;
	req.num_lines = 1;
	strncpy(req.consumer, "cap-sonar", sizeof(req.consumer) - 1);
	if (output) {
		req.config.flags = GPIO_V2_LINE_FLAG_OUTPUT;	/* starts low */
	} else {
		req.config.flags = GPIO_V2_LINE_FLAG_INPUT | GPIO_V2_LINE_FLAG_EDGE_RISING |
				   GPIO_V2_LINE_FLAG_EDGE_FALLING;	/* default clock: MONOTONIC */
		req.event_buffer_size = EDGE_BUF;
	}
	if (ioctl(chip, GPIO_V2_GET_LINE_IOCTL, &req) < 0)
		return -1;
	return req.fd;	/* the kernel opens it close-on-exec */
}

static int line_set(int fd, bool high)
{
	struct gpio_v2_line_values v = { .bits = high ? 1 : 0, .mask = 1 };

	return ioctl(fd, GPIO_V2_LINE_SET_VALUES_IOCTL, &v);
}

static int line_get(int fd, bool *high)
{
	struct gpio_v2_line_values v = { .bits = 0, .mask = 1 };

	if (ioctl(fd, GPIO_V2_LINE_GET_VALUES_IOCTL, &v) < 0)
		return -1;
	*high = v.bits & 1;
	return 0;
}

/* Reads every queued edge; keeps those from the current ping. */
static int drain_edges(struct sonar *s, int64_t now)
{
	struct gpio_v2_line_event ev[EDGE_BUF];
	ssize_t n;
	size_t i;

	for (;;) {
		n = read(s->echo_fd, ev, sizeof(ev));
		if (n < 0) {
			if (errno == EAGAIN || errno == EWOULDBLOCK)
				return 0;
			if (errno == EINTR)
				continue;
			return -1;
		}
		if (n == 0 || n % (ssize_t)sizeof(ev[0]) != 0) {
			errno = EIO;
			return -1;
		}
		for (i = 0; i < (size_t)n / sizeof(ev[0]); i++) {
			if (!s->armed || (int64_t)ev[i].timestamp_ns < s->trig_ns)
				continue;	/* before this ping's trigger */
			if (s->nedge == MAX_EDGES) {
				s->edges_dropped++;
				log_limited(&s->lim_edges, now, LOG_W, "edges_dropped max=%d", MAX_EDGES);
				continue;
			}
			s->edge[s->nedge].ts_ns = (int64_t)ev[i].timestamp_ns;
			s->edge[s->nedge].rising = ev[i].id == GPIO_V2_LINE_EVENT_RISING_EDGE;
			s->nedge++;
		}
	}
}

static void send_ping(struct sonar *s, enum echo_result res, const struct echo_ping *p, int64_t now)
{
	struct sonar_msg m;

	memset(&m, 0, sizeof(m));
	m.type = MSG_SONAR;
	m.seq = s->seq++;
	m.trigger_seq = s->trigger_seq;
	m.origin_ns = (uint64_t)p->rise_ns;
	if (res == ECHO_OK)
		m.echo_ns = p->width_ns;
	else
		m.flags = SONAR_NO_ECHO;
	if (ipc_send(&s->hub, &m, sizeof(m), now) == 0)
		s->sent++;
	else
		log_limited(&s->lim_send, now, LOG_W, "send_dropped err=%s", strerror(errno));
}

/* Ends the ping in flight: classify, send or count the miss. */
static int finish_ping(struct sonar *s, int64_t now)
{
	struct echo_ping p = { 0 };
	enum echo_result res;
	bool high;

	if (!s->armed)
		return 0;
	if (line_get(s->echo_fd, &high) < 0)
		return -1;
	res = echo_classify(s->edge, s->nedge, high, &p);
	s->armed = false;
	s->nedge = 0;

	if (res == ECHO_NONE || res == ECHO_STUCK) {
		s->missed++;
		if (++s->miss_run == LOSS_PINGS && !s->lost) {
			log_msg(LOG_W, "sonar_lost reason=%s pings=%d", echo_result_name(res), LOSS_PINGS);
			s->lost = true;
		}
		return 0;
	}
	if (!s->up)
		log_msg(LOG_I, "sonar_up");
	else if (s->lost)
		log_msg(LOG_I, "sonar_back missed=%d", s->miss_run);
	s->up = true;
	s->lost = false;
	s->miss_run = 0;
	if (res == ECHO_FAR)
		s->far++;
	send_ping(s, res, &p, now);
	return 0;
}

/* TRIG high for >= 10 us. The pulse may run long if we're preempted; the
 * HC-SR04 only needs the falling edge. */
static int trigger(struct sonar *s)
{
	struct timespec pulse = { .tv_sec = 0, .tv_nsec = TRIG_PULSE_NS };
	int64_t t;
	int r;

	t = dlby_now_ns();
	if (t < 0 || line_set(s->trig_fd, true) < 0)
		return -1;
	s->trig_ns = t;
	s->armed = true;
	s->trigger_seq++;
	while ((r = clock_nanosleep(CLOCK_MONOTONIC, 0, &pulse, &pulse)) == EINTR)
		;
	if (r != 0) {
		errno = r;
		(void)line_set(s->trig_fd, false);
		return -1;
	}
	return line_set(s->trig_fd, false);
}

static int signal_fd(void)
{
	sigset_t mask;

	if (sigemptyset(&mask) < 0 || sigaddset(&mask, SIGTERM) < 0 || sigaddset(&mask, SIGINT) < 0)
		return -1;
	if (sigprocmask(SIG_BLOCK, &mask, NULL) < 0)
		return -1;
	return signalfd(-1, &mask, SFD_CLOEXEC);
}

static int timer_fd(int64_t start_ns)
{
	struct itimerspec its = {
		.it_value    = { .tv_sec = start_ns / 1000000000, .tv_nsec = start_ns % 1000000000 },
		.it_interval = { .tv_sec = 0, .tv_nsec = SONAR_PERIOD_NS },
	};
	int fd;

	fd = timerfd_create(CLOCK_MONOTONIC, TFD_CLOEXEC | TFD_NONBLOCK);
	if (fd < 0)
		return -1;
	if (timerfd_settime(fd, TFD_TIMER_ABSTIME, &its, NULL) < 0) {
		int e = errno;

		(void)close(fd);
		errno = e;
		return -1;
	}
	return fd;
}

int main(void)
{
	struct sonar s;
	struct hb hb;
	struct pollfd pfd[4];
	struct signalfd_siginfo si;
	const char *chip_path = getenv(CHIP_ENV);
	unsigned trig, echo;
	uint64_t expirations;
	int64_t now;
	int sfd = -1, tfd = -1, chip = -1, rc = EX_SOFTWARE, supervised;
	ssize_t n;

	log_init("cap-sonar");
	memset(&s, 0, sizeof(s));
	s.trig_fd = -1;
	s.echo_fd = -1;
	s.hub.fd = -1;		/* an early goto out must not close fd 0 */
	if (!chip_path || !*chip_path)
		chip_path = CHIP_DEFAULT;

	supervised = hb_init(&hb);
	if (supervised < 0) {
		log_msg(LOG_E, "hb_init_failed err=%s", strerror(errno));
		goto out;
	}
	/* DESIGN.md §2 C. Unprivileged bench runs can't lock memory: warn, carry on. */
	if (supervised && mlockall(MCL_CURRENT | MCL_FUTURE) < 0)
		log_msg(LOG_W, "mlockall_failed err=%s", strerror(errno));
	if (parse_line(TRIG_ENV, TRIG_DEFAULT, &trig) < 0 || parse_line(ECHO_ENV, ECHO_DEFAULT, &echo) < 0 ||
	    trig == echo) {
		log_msg(LOG_E, "bad_config trig=%s echo=%s", getenv(TRIG_ENV) ? getenv(TRIG_ENV) : "-",
			getenv(ECHO_ENV) ? getenv(ECHO_ENV) : "-");
		rc = EX_CONFIG;
		goto out;
	}
	sfd = signal_fd();
	if (sfd < 0) {
		log_msg(LOG_E, "signalfd_failed err=%s", strerror(errno));
		goto out;
	}
	if (ipc_conn_init(&s.hub, "fuse-hub") < 0) {
		log_msg(LOG_E, "bad_sock_path err=%s", strerror(errno));
		rc = EX_CONFIG;
		goto out;
	}

	chip = open(chip_path, O_RDWR | O_CLOEXEC);
	if (chip < 0) {
		log_msg(LOG_E, "open_failed chip=%s err=%s", chip_path, strerror(errno));
		rc = errno == ENOENT || errno == EACCES || errno == ENOTDIR ? EX_CONFIG : EX_UNAVAILABLE;
		goto out;
	}
	s.trig_fd = request_line(chip, trig, true);
	if (s.trig_fd < 0) {
		log_msg(LOG_E, "line_request_failed chip=%s line=%u err=%s", chip_path, trig, strerror(errno));
		rc = errno == EBUSY ? EX_UNAVAILABLE : EX_CONFIG;
		goto out;
	}
	s.echo_fd = request_line(chip, echo, false);
	if (s.echo_fd < 0) {
		log_msg(LOG_E, "line_request_failed chip=%s line=%u err=%s", chip_path, echo, strerror(errno));
		rc = errno == EBUSY ? EX_UNAVAILABLE : EX_CONFIG;
		goto out;
	}
	if (close(chip) < 0)
		log_msg(LOG_W, "close_failed err=%s", strerror(errno));
	chip = -1;
	if (fcntl(s.echo_fd, F_SETFL, O_NONBLOCK) < 0) {
		log_msg(LOG_E, "fcntl_failed err=%s", strerror(errno));
		goto out;
	}

	now = dlby_now_ns();
	if (now < 0) {
		log_msg(LOG_E, "clock_failed err=%s", strerror(errno));
		goto out;
	}
	tfd = timer_fd(now + SONAR_PERIOD_NS);
	if (tfd < 0) {
		log_msg(LOG_E, "timerfd_failed err=%s", strerror(errno));
		goto out;
	}
	log_msg(LOG_I, "start chip=%s trig=%u echo=%u", chip_path, trig, echo);

	for (;;) {
		pfd[0] = (struct pollfd){ .fd = sfd, .events = POLLIN };
		pfd[1] = (struct pollfd){ .fd = tfd, .events = POLLIN };
		pfd[2] = (struct pollfd){ .fd = s.echo_fd, .events = POLLIN };
		pfd[3] = (struct pollfd){ .fd = hb.fd, .events = POLLIN };	/* -1: ignored */

		if (poll(pfd, 4, POLL_MS) < 0) {
			if (errno == EINTR)
				continue;
			log_msg(LOG_E, "poll_failed err=%s", strerror(errno));
			goto out;
		}
		now = dlby_now_ns();
		if (now < 0) {
			log_msg(LOG_E, "clock_failed err=%s", strerror(errno));
			goto out;
		}

		if (pfd[0].revents) {
			n = read(sfd, &si, sizeof(si));
			if (n == (ssize_t)sizeof(si)) {
				rc = EX_OK;
				goto out;
			}
			if (n < 0 && (errno == EINTR || errno == EAGAIN))
				continue;
			log_msg(LOG_E, "signalfd_read_failed err=%s", n < 0 ? strerror(errno) : "short");
			goto out;
		}
		if (pfd[2].revents && drain_edges(&s, now) < 0) {
			log_msg(LOG_E, "edge_read_failed err=%s", strerror(errno));
			rc = EX_UNAVAILABLE;
			goto out;
		}
		if (pfd[1].revents) {
			n = read(tfd, &expirations, sizeof(expirations));
			if (n < 0 && (errno == EAGAIN || errno == EINTR))
				continue;
			if (n != (ssize_t)sizeof(expirations)) {
				log_msg(LOG_E, "timerfd_read_failed err=%s", n < 0 ? strerror(errno) : "short");
				goto out;
			}
			if (expirations > 1) {
				s.overruns += expirations - 1;
				log_limited(&s.lim_overrun, now, LOG_W, "overrun missed=%" PRIu64,
					    expirations - 1);
			}
			/* Edges queued since the last poll belong to the ping now ending. */
			if (drain_edges(&s, now) < 0 || finish_ping(&s, now) < 0 || trigger(&s) < 0) {
				log_msg(LOG_E, "gpio_failed err=%s", strerror(errno));
				rc = EX_UNAVAILABLE;
				goto out;
			}
		}

		/* From this loop, never a side thread: a stuck loop goes quiet. */
		if ((pfd[3].revents && hb_readable(&hb) < 0) || hb_tick(&hb, now) < 0) {
			log_msg(LOG_W, "super_lost err=%s", strerror(errno));
			rc = EX_OK;
			goto out;
		}
	}

out:
	if (s.trig_fd >= 0) {
		if (line_set(s.trig_fd, false) < 0)
			log_msg(LOG_W, "trig_low_failed err=%s", strerror(errno));
		if (close(s.trig_fd) < 0)
			log_msg(LOG_W, "close_failed err=%s", strerror(errno));
	}
	if (s.echo_fd >= 0 && close(s.echo_fd) < 0)
		log_msg(LOG_W, "close_failed err=%s", strerror(errno));
	if (chip >= 0 && close(chip) < 0)
		log_msg(LOG_W, "close_failed err=%s", strerror(errno));
	if (tfd >= 0 && close(tfd) < 0)
		log_msg(LOG_W, "close_failed err=%s", strerror(errno));
	if (sfd >= 0 && close(sfd) < 0)
		log_msg(LOG_W, "close_failed err=%s", strerror(errno));
	ipc_conn_close(&s.hub);
	log_msg(rc == EX_OK ? LOG_I : LOG_E, "exit code=%d pings=%" PRIu32 " sent=%" PRIu64 " far=%" PRIu64
		" missed=%" PRIu64 " overruns=%" PRIu64 " edges_dropped=%" PRIu64 " dropped=%" PRIu64,
		rc, s.trigger_seq, s.sent, s.far, s.missed, s.overruns, s.edges_dropped, s.hub.dropped);
	return rc;
}
