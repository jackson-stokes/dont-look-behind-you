/* Fake cap-radar / cap-sonar for testing fuse-hub without hardware. Never deployed.
 *
 *   fake-sensors radar|sonar <send_s> [hold_s]
 *
 * Sends at the real rate (health.h) on absolute deadlines for send_s seconds, then
 * keeps the connection open and silent for hold_s (default 0) so the hub sees a
 * stale sensor rather than an EOF.
 *
 * Demo mode: run with no arguments as "cap-radar" or "cap-sonar" (a symlink), it
 * takes the sensor from its name, sends until SIGTERM and heartbeats to
 * dlby-super (hb.h), so super can run it in place of the real capture process.
 *
 * Exit: 0 (also on SIGTERM or super gone in demo mode), 64 usage, 78 bad socket
 * path, 70 other.
 */
#include "hb.h"
#include "health.h"
#include "ipc.h"
#include "log.h"
#include "msg.h"

#include <errno.h>
#include <inttypes.h>
#include <signal.h>
#include <stdlib.h>
#include <string.h>
#include <sysexits.h>
#include <time.h>

#define SEC_NS 1000000000LL

/* Round trip to a target 1.5 m away at 343 m/s. */
#define ECHO_1M5_NS ((uint32_t)(2.0 * 1.5 / 343.0 * 1e9))

static volatile sig_atomic_t stop;

static void on_term(int sig)
{
	(void)sig;
	stop = 1;
}

static int parse_secs(const char *s, int64_t *ns)
{
	char *end;
	double v;

	errno = 0;
	v = strtod(s, &end);
	if (errno || end == s || *end != '\0' || v < 0 || v > 3600)
		return -1;
	*ns = (int64_t)(v * SEC_NS);
	return 0;
}

/* Returns 0 or an errno value, as clock_nanosleep does; EINTR once stop is set. */
static int sleep_until(int64_t t_ns)
{
	struct timespec ts = { .tv_sec = t_ns / SEC_NS, .tv_nsec = t_ns % SEC_NS };
	int r;

	do {
		r = clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &ts, NULL);
	} while (r == EINTR && !stop);
	return r == EINTR ? 0 : r;
}

static void fill_radar(struct radar_msg *m, uint32_t seq, int64_t now)
{
	int range_cm = 5000 - 50 * (int)(seq % 98);

	memset(m, 0, sizeof(*m));
	m->type = MSG_RADAR;
	m->seq = seq;
	m->origin_ns = (uint64_t)now;
	m->ntargets = 1;
	m->t[0].range_cm = (uint16_t)(range_cm > 100 ? range_cm : 100);
	m->t[0].speed_cms = -500;
}

static void fill_sonar(struct sonar_msg *m, uint32_t seq, int64_t now, bool echo)
{
	memset(m, 0, sizeof(*m));
	m->type = MSG_SONAR;
	m->seq = seq;
	m->origin_ns = (uint64_t)now;
	m->trigger_seq = seq;
	if (echo)
		m->echo_ns = ECHO_1M5_NS;
	else
		m->flags = SONAR_NO_ECHO;
}

int main(int argc, char **argv)
{
	struct ipc_conn c;
	struct hb hb;
	struct log_limit lim = { 0 };
	struct sigaction sa;
	struct radar_msg rm;
	struct sonar_msg sm;
	const char *base = strrchr(argv[0], '/') ? strrchr(argv[0], '/') + 1 : argv[0];
	int64_t send_ns = 0, hold_ns = 0, period, start, end, deadline, now;
	uint64_t sent = 0;
	uint32_t seq;
	bool radar, demo;
	int r;

	demo = argc == 1 && (strcmp(base, "cap-radar") == 0 || strcmp(base, "cap-sonar") == 0);
	if (!demo && (argc < 3 || argc > 4 || (strcmp(argv[1], "radar") && strcmp(argv[1], "sonar")) ||
		      parse_secs(argv[2], &send_ns) < 0 ||
		      (argc == 4 && parse_secs(argv[3], &hold_ns) < 0))) {
		log_init("fake-sensors");
		log_msg(LOG_E, "usage args=\"radar|sonar <send_s> [hold_s]\"");
		return EX_USAGE;
	}
	radar = demo ? strcmp(base, "cap-radar") == 0 : strcmp(argv[1], "radar") == 0;
	log_init(demo ? base : radar ? "fake-radar" : "fake-sonar");
	if (hb_init(&hb) < 0) {
		log_msg(LOG_E, "hb_init_failed err=%s", strerror(errno));
		return EX_SOFTWARE;
	}
	memset(&sa, 0, sizeof(sa));
	sa.sa_handler = on_term;	/* no SA_RESTART: interrupt the sleep */
	if (sigemptyset(&sa.sa_mask) < 0 || sigaction(SIGTERM, &sa, NULL) < 0 ||
	    sigaction(SIGINT, &sa, NULL) < 0) {
		log_msg(LOG_E, "sigaction_failed err=%s", strerror(errno));
		return EX_SOFTWARE;
	}
	if (ipc_conn_init(&c, "fuse-hub") < 0) {
		log_msg(LOG_E, "bad_sock_path err=%s", strerror(errno));
		return EX_CONFIG;
	}

	period = radar ? RADAR_PERIOD_NS : SONAR_PERIOD_NS;
	start = dlby_now_ns();
	if (start < 0)
		goto clock_fail;
	end = demo ? INT64_MAX : start + send_ns;

	for (seq = 0, deadline = start; deadline < end && !stop; seq++, deadline += period) {
		now = dlby_now_ns();
		if (now < 0)
			goto clock_fail;
		if (hb_readable(&hb) < 0 || hb_tick(&hb, now) < 0) {
			log_msg(LOG_W, "super_lost err=%s", strerror(errno));
			break;
		}
		if (radar) {
			fill_radar(&rm, seq, now);
			r = ipc_send(&c, &rm, sizeof(rm), now);
		} else {
			fill_sonar(&sm, seq, now, !demo && end - now <= SEC_NS);
			r = ipc_send(&c, &sm, sizeof(sm), now);
		}
		if (r == 0)
			sent++;
		else
			log_limited(&lim, now, LOG_W, "send_dropped err=%s", strerror(errno));
		r = sleep_until(deadline + period);
		if (r != 0)
			goto sleep_fail;
	}

	if (!demo && !stop) {
		r = sleep_until(end + hold_ns);
		if (r != 0)
			goto sleep_fail;
	}
	log_msg(LOG_I, "done sent=%" PRIu64 " dropped=%" PRIu64, sent, c.dropped);
	ipc_conn_close(&c);
	return EX_OK;

clock_fail:
	log_msg(LOG_E, "clock_failed err=%s", strerror(errno));
	ipc_conn_close(&c);
	return EX_SOFTWARE;
sleep_fail:
	log_msg(LOG_E, "sleep_failed err=%s", strerror(r));
	ipc_conn_close(&c);
	return EX_SOFTWARE;
}
