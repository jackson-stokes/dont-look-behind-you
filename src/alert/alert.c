/* alert: drives the two side-mirror LEDs and the buzzer from fuse-hub's alert
 * messages (DESIGN.md §1, §2 E). Each LED shows its own side's state
 * (alert_msg.state: right, .state_left: left); the buzzer plays the side whose
 * buzzer matters most (pattern.h).
 *
 * - Listens on alert.sock. Starts in HUB_LOST ("not protected") until the first
 *   message; after HUB_LOST_NS without one (fuse-hub refreshes at 2 Hz) both
 *   sides go back to HUB_LOST: "hub_lost", then "hub_back" when messages resume.
 * - A state change drives the outputs in the same loop pass; pattern edges are
 *   scheduled on absolute times from each side's entry, so they don't drift.
 * - Outputs through gpio_out.h ($DLBY_ALERT_CHIP=none: no GPIO). All low on exit.
 * - $DLBY_ALERT_TRACE=<file> (bench and tests only): one line per output change,
 *   "t_ms led_right led_left buzzer", t_ms from start.
 * - Heartbeats to dlby-super from the main loop (hb.h); counts every
 *   $DLBY_REPORT_S and a final=1 line on exit (report.h).
 *
 * Exit codes: 0 on SIGTERM/SIGINT or when dlby-super is gone, 78 on a bad setting
 * or a missing GPIO chip, 69 if the lines are busy or the socket can't be set
 * up, 70 on any other unexpected failure.
 */
#include "gpio_out.h"
#include "hb.h"
#include "ipc.h"
#include "log.h"
#include "msg.h"
#include "pattern.h"
#include "report.h"

#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/signalfd.h>
#include <sysexits.h>
#include <unistd.h>

#define MAX_CLIENTS 4
#define MAX_IDLE_MS 1000
#define HUB_LOST_NS 1000000000LL
#define TRACE_ENV   "DLBY_ALERT_TRACE"

enum { SIDE_R = 0, SIDE_L = 1 };

struct alert {
	struct alert_pins pins;
	int      gpio_fd;		/* -1 with no GPIO */
	int      trace_fd;		/* -1 without a trace */
	int      cl[MAX_CLIENTS];	/* -1 if free */
	unsigned state[2];		/* enum fuse_state or PAT_HUB_LOST, per side */
	int64_t  entry_ns[2];
	unsigned out;			/* AL_BIT levels last written */
	bool     out_known;
	bool     have_hub, hub_lost;
	int64_t  last_rx_ns, start_ns;
	uint64_t msgs, rejected, hub_lost_n, changes[2], writes, write_errs;
	struct log_limit lim_bad, lim_full, lim_accept, lim_write, lim_trace;
};

static const char *const side_name[2] = { "right", "left" };

static void trace(struct alert *a, int64_t now, unsigned bits)
{
	char line[64];
	ssize_t w;
	int n;

	if (a->trace_fd < 0)
		return;
	n = snprintf(line, sizeof(line), "%" PRId64 " %u %u %u\n", (now - a->start_ns) / 1000000,
		     !!(bits & AL_BIT(AL_LED_RIGHT)), !!(bits & AL_BIT(AL_LED_LEFT)),
		     !!(bits & AL_BIT(AL_BUZZ)));
	w = write(a->trace_fd, line, (size_t)n);
	if (w != n)
		log_limited(&a->lim_trace, now, LOG_W, "trace_write_failed err=%s",
			    w < 0 ? strerror(errno) : "short");
}

/* What the outputs should be now. */
static unsigned levels(const struct alert *a, int64_t now)
{
	int b = pattern_buzz_side(a->state[SIDE_R], a->state[SIDE_L]);
	unsigned bits = 0;

	if (pattern_led(a->state[SIDE_R], now - a->entry_ns[SIDE_R]))
		bits |= AL_BIT(AL_LED_RIGHT);
	if (pattern_led(a->state[SIDE_L], now - a->entry_ns[SIDE_L]))
		bits |= AL_BIT(AL_LED_LEFT);
	if (pattern_buzz(a->state[b], now - a->entry_ns[b]))
		bits |= AL_BIT(AL_BUZZ);
	return bits;
}

/* Write the outputs if they changed. A failed write leaves out as it was, so
 * the next pass (at most MAX_IDLE_MS later) tries again. */
static void drive(struct alert *a, int64_t now, unsigned bits)
{
	if (a->out_known && bits == a->out)
		return;
	if (a->gpio_fd >= 0 && gpio_out_set(a->gpio_fd, bits) < 0) {
		a->write_errs++;
		log_limited(&a->lim_write, now, LOG_E, "gpio_write_failed err=%s", strerror(errno));
		return;
	}
	a->out = bits;
	a->out_known = true;
	a->writes++;
	trace(a, now, bits);
}

static void set_state(struct alert *a, int side, unsigned st, int64_t now)
{
	if (st == a->state[side])
		return;
	a->state[side] = st;
	a->entry_ns[side] = now;
	a->changes[side]++;
	log_msg(LOG_I, "pattern side=%s state=%s t_ms=%" PRId64, side_name[side],
		pattern_state_name(st), (now - a->start_ns) / 1000000);
}

/* The earliest pattern edge of either side, absolute. */
static int64_t next_edge(const struct alert *a, int64_t now)
{
	int64_t best = INT64_MAX, e;
	int s;

	for (s = 0; s < 2; s++) {
		e = pattern_next_edge(a->state[s], now - a->entry_ns[s]);
		if (e != INT64_MAX && a->entry_ns[s] + e < best)
			best = a->entry_ns[s] + e;
	}
	return best;
}

static void check_hub(struct alert *a, int64_t now)
{
	if (!a->have_hub || a->hub_lost || now - a->last_rx_ns < HUB_LOST_NS)
		return;
	a->hub_lost = true;
	a->hub_lost_n++;
	log_msg(LOG_W, "hub_lost silent_ms=%" PRId64, (now - a->last_rx_ns) / 1000000);
	set_state(a, SIDE_R, PAT_HUB_LOST, now);
	set_state(a, SIDE_L, PAT_HUB_LOST, now);
}

static void close_client(struct alert *a, int i)
{
	/* close() releases the fd on Linux even when it reports an error. */
	if (close(a->cl[i]) < 0)
		log_msg(LOG_W, "close_failed err=%s", strerror(errno));
	a->cl[i] = -1;
}

/* Returns -1 only on an unexpected accept() failure. */
static int accept_client(struct alert *a, int lfd, int64_t now)
{
	int fd, i;

	fd = ipc_accept(lfd);
	if (fd < 0) {
		if (errno == EAGAIN || errno == EWOULDBLOCK || errno == ECONNABORTED || errno == EINTR)
			return 0;
		if (errno == EMFILE || errno == ENFILE || errno == ENOBUFS || errno == ENOMEM) {
			log_limited(&a->lim_accept, now, LOG_W, "accept_failed err=%s", strerror(errno));
			return 0;
		}
		log_msg(LOG_E, "accept_failed err=%s", strerror(errno));
		return -1;
	}
	for (i = 0; i < MAX_CLIENTS; i++) {
		if (a->cl[i] < 0) {
			a->cl[i] = fd;
			return 0;
		}
	}
	if (close(fd) < 0)
		log_msg(LOG_W, "close_failed err=%s", strerror(errno));
	log_limited(&a->lim_full, now, LOG_W, "too_many_clients max=%d", MAX_CLIENTS);
	return 0;
}

static void read_client(struct alert *a, int i, int64_t now)
{
	uint8_t buf[MSG_MAX];
	struct alert_msg m;
	ssize_t n;
	int type;

	n = ipc_recv(a->cl[i], buf, sizeof(buf), &a->rejected);
	if (n < 0) {
		if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR)
			return;
		if (errno == EMSGSIZE) {
			log_limited(&a->lim_bad, now, LOG_W, "bad_msg reason=trunc");
			return;
		}
		log_msg(LOG_W, "recv_failed err=%s", strerror(errno));
		close_client(a, i);
		return;
	}
	if (n == 0) {
		close_client(a, i);
		return;
	}
	type = ipc_check_msg(buf, (size_t)n, &a->rejected);
	if (type < 0) {
		log_limited(&a->lim_bad, now, LOG_W, "bad_msg reason=type len=%zd", n);
		return;
	}
	if (type != MSG_ALERT) {
		a->rejected++;
		log_limited(&a->lim_bad, now, LOG_W, "bad_msg reason=unexpected type=%d", type);
		return;
	}
	memcpy(&m, buf, sizeof(m));	/* length checked by ipc_check_msg */
	if (m.state > ST_DEG_IN_ZONE || m.state_left > ST_DEG_IN_ZONE) {
		a->rejected++;
		log_limited(&a->lim_bad, now, LOG_W, "bad_msg reason=state state=%u state_left=%u",
			    (unsigned)m.state, (unsigned)m.state_left);
		return;
	}
	a->msgs++;
	a->last_rx_ns = now;
	if (!a->have_hub) {
		a->have_hub = true;
		log_msg(LOG_I, "hub_up");
	} else if (a->hub_lost) {
		log_msg(LOG_I, "hub_back");
	}
	a->hub_lost = false;
	set_state(a, SIDE_R, m.state, now);
	set_state(a, SIDE_L, m.state_left, now);
}

static void log_counts(const struct alert *a, const struct report *r, bool final)
{
	log_msg(LOG_I, "counts n=%" PRIu32 " msgs=%" PRIu64 " rejected=%" PRIu64 " hub_lost=%" PRIu64
		" changes_right=%" PRIu64 " changes_left=%" PRIu64 " writes=%" PRIu64
		" write_errs=%" PRIu64 " right=%s left=%s%s", r->n, a->msgs, a->rejected,
		a->hub_lost_n, a->changes[SIDE_R], a->changes[SIDE_L], a->writes, a->write_errs,
		pattern_state_name(a->state[SIDE_R]), pattern_state_name(a->state[SIDE_L]),
		final ? " final=1" : "");
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

static int timeout_ms(int64_t due, int64_t now)
{
	int64_t ms;

	if (due == INT64_MAX)
		return MAX_IDLE_MS;
	ms = (due - now + 999999) / 1000000;
	return ms < 0 ? 0 : ms > MAX_IDLE_MS ? MAX_IDLE_MS : (int)ms;
}

int main(void)
{
	struct alert a;
	struct hb hb;
	struct report rep = { .next_ns = INT64_MAX };
	struct pollfd pfd[3 + MAX_CLIENTS];
	int slot[3 + MAX_CLIENTS];
	struct signalfd_siginfo si;
	char path[IPC_PATH_MAX], pins[32];
	const char *trace_path;
	bool rep_ok = false;
	int64_t now, due, t;
	int sfd = -1, lfd = -1, rc = EX_SOFTWARE, supervised, i, n, nfds;
	ssize_t r;

	log_init("alert");
	memset(&a, 0, sizeof(a));
	a.gpio_fd = -1;
	a.trace_fd = -1;
	for (i = 0; i < MAX_CLIENTS; i++)
		a.cl[i] = -1;

	supervised = hb_init(&hb);
	if (supervised < 0) {
		log_msg(LOG_E, "hb_init_failed err=%s", strerror(errno));
		goto out;
	}
	/* DESIGN.md §2 C. Unprivileged bench runs can't lock memory: warn, carry on. */
	if (supervised && mlockall(MCL_CURRENT | MCL_FUTURE) < 0)
		log_msg(LOG_W, "mlockall_failed err=%s", strerror(errno));
	if (alert_pins_from_env(&a.pins) < 0) {
		log_msg(LOG_E, "bad_config %s=%s %s=%s %s=%s %s=%s", ALERT_CHIP_ENV,
			getenv(ALERT_CHIP_ENV) ? getenv(ALERT_CHIP_ENV) : "-", ALERT_LED_RIGHT_ENV,
			getenv(ALERT_LED_RIGHT_ENV) ? getenv(ALERT_LED_RIGHT_ENV) : "-",
			ALERT_LED_LEFT_ENV, getenv(ALERT_LED_LEFT_ENV) ? getenv(ALERT_LED_LEFT_ENV) : "-",
			ALERT_BUZZ_ENV, getenv(ALERT_BUZZ_ENV) ? getenv(ALERT_BUZZ_ENV) : "-");
		rc = EX_CONFIG;
		goto out;
	}
	alert_pins_str(&a.pins, pins, sizeof(pins));
	sfd = signal_fd();
	if (sfd < 0) {
		log_msg(LOG_E, "signalfd_failed err=%s", strerror(errno));
		goto out;
	}
	now = dlby_now_ns();
	if (now < 0) {
		log_msg(LOG_E, "clock_failed err=%s", strerror(errno));
		goto out;
	}
	if (report_init(&rep, now) < 0) {
		log_msg(LOG_E, "bad_config %s=%s want=1..%d", REPORT_ENV, getenv(REPORT_ENV),
			REPORT_MAX_S);
		rc = EX_CONFIG;
		goto out;
	}
	rep_ok = true;
	if (!a.pins.none) {
		a.gpio_fd = gpio_out_open(&a.pins, "alert");
		if (a.gpio_fd < 0) {
			log_msg(LOG_E, "gpio_open_failed chip=%s lines=%s err=%s", a.pins.chip, pins,
				strerror(errno));
			rc = gpio_out_exit_code(errno);
			goto out;
		}
	}
	trace_path = getenv(TRACE_ENV);
	if (trace_path && *trace_path) {
		a.trace_fd = open(trace_path, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
		if (a.trace_fd < 0) {
			log_msg(LOG_E, "trace_open_failed path=%s err=%s", trace_path, strerror(errno));
			rc = EX_CONFIG;
			goto out;
		}
	}
	if (ipc_path("alert", path, sizeof(path)) < 0) {
		log_msg(LOG_E, "listen_failed err=%s", strerror(errno));
		rc = EX_UNAVAILABLE;
		goto out;
	}
	lfd = ipc_listen("alert");
	if (lfd < 0) {
		log_msg(LOG_E, "listen_failed sock=%s err=%s", path, strerror(errno));
		rc = EX_UNAVAILABLE;
		goto out;
	}

	a.start_ns = now;
	a.state[SIDE_R] = a.state[SIDE_L] = PAT_HUB_LOST;
	a.entry_ns[SIDE_R] = a.entry_ns[SIDE_L] = now;
	log_msg(LOG_I, "start sock=%s gpio=%s trace=%s", path, pins,
		a.trace_fd >= 0 ? trace_path : "-");
	drive(&a, now, levels(&a, now));

	for (;;) {
		pfd[0] = (struct pollfd){ .fd = sfd, .events = POLLIN };
		pfd[1] = (struct pollfd){ .fd = lfd, .events = POLLIN };
		pfd[2] = (struct pollfd){ .fd = hb.fd, .events = POLLIN };	/* -1: ignored */
		nfds = 3;
		for (i = 0; i < MAX_CLIENTS; i++) {
			if (a.cl[i] >= 0) {
				slot[nfds] = i;
				pfd[nfds++] = (struct pollfd){ .fd = a.cl[i], .events = POLLIN };
			}
		}
		due = next_edge(&a, now);
		if (a.have_hub && !a.hub_lost && a.last_rx_ns + HUB_LOST_NS < due)
			due = a.last_rx_ns + HUB_LOST_NS;
		if (hb_due_ns(&hb) < due)
			due = hb_due_ns(&hb);
		if (rep.next_ns < due)
			due = rep.next_ns;

		n = poll(pfd, (nfds_t)nfds, timeout_ms(due, now));
		if (n < 0 && errno != EINTR) {
			log_msg(LOG_E, "poll_failed err=%s", strerror(errno));
			goto out;
		}
		now = dlby_now_ns();
		if (now < 0) {
			log_msg(LOG_E, "clock_failed err=%s", strerror(errno));
			goto out;
		}

		if (n > 0) {
			if (pfd[0].revents) {
				r = read(sfd, &si, sizeof(si));
				if (r == (ssize_t)sizeof(si)) {
					rc = EX_OK;
					goto out;
				}
				if (r >= 0 || (errno != EINTR && errno != EAGAIN)) {
					log_msg(LOG_E, "signalfd_read_failed err=%s",
						r < 0 ? strerror(errno) : "short");
					goto out;
				}
			}
			if ((pfd[1].revents & POLLIN) && accept_client(&a, lfd, now) < 0)
				goto out;
			for (i = 3; i < nfds; i++)
				if (pfd[i].revents && a.cl[slot[i]] >= 0)
					read_client(&a, slot[i], now);
		}

		check_hub(&a, now);
		drive(&a, now, levels(&a, now));

		/* From this loop, never a side thread: a stuck loop goes quiet. */
		if ((pfd[2].revents && hb_readable(&hb) < 0) || hb_tick(&hb, now) < 0) {
			log_msg(LOG_W, "super_lost err=%s", strerror(errno));
			rc = EX_OK;
			goto out;
		}
		if (report_due(&rep, now))
			log_counts(&a, &rep, false);
	}

out:
	/* Outputs off: a dark LED must mean "not running", never a stale warning. */
	if (a.start_ns && (!a.out_known || a.out)) {
		t = dlby_now_ns();
		if (a.gpio_fd >= 0 && gpio_out_set(a.gpio_fd, 0) < 0)
			log_msg(LOG_E, "gpio_write_failed err=%s", strerror(errno));
		trace(&a, t >= 0 ? t : a.start_ns, 0);
	}
	if (a.gpio_fd >= 0 && close(a.gpio_fd) < 0)
		log_msg(LOG_W, "close_failed err=%s", strerror(errno));
	if (a.trace_fd >= 0 && close(a.trace_fd) < 0)
		log_msg(LOG_W, "close_failed err=%s", strerror(errno));
	for (i = 0; i < MAX_CLIENTS; i++)
		if (a.cl[i] >= 0)
			close_client(&a, i);
	if (lfd >= 0 && close(lfd) < 0)
		log_msg(LOG_W, "close_failed err=%s", strerror(errno));
	if (sfd >= 0 && close(sfd) < 0)
		log_msg(LOG_W, "close_failed err=%s", strerror(errno));
	if (rep_ok) {
		report_final(&rep);
		log_counts(&a, &rep, true);
	}
	log_msg(rc == EX_OK ? LOG_I : LOG_E, "exit code=%d", rc);
	return rc;
}
