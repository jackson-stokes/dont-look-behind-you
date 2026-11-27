/* Fake cap-radar / cap-radar-left / cap-sonar for testing fuse-hub without
 * hardware. Never deployed.
 *
 *   fake-sensors radar|radar_left|sonar <send_s> [hold_s]
 *
 * Sends at the real rate (health.h) on absolute deadlines for send_s seconds, then
 * keeps the connection open and silent for hold_s (default 0) so the hub sees a
 * stale sensor rather than an EOF. The radar's one target closes from 50 m at
 * 5 m/s down to $FAKE_RADAR_MIN_CM (100..5000, default 100), then starts again.
 *
 * Demo mode: run with no arguments as "cap-radar", "cap-radar-left" or "cap-sonar"
 * (a symlink), it
 * takes the sensor from its name, sends until SIGTERM and heartbeats to
 * dlby-super (hb.h), so super can run it in place of the real capture process.
 *
 * Pass mode ($FAKE_PASS_S = period in seconds, 7..3600; either mode above): a
 * cyclist closing at 20 km/h from 30 m, once per period, as the LD2451 and the
 * HC-SR04 would report it (docs/evidence/radar-targets-pi.txt):
 *   radar  ticks at 10 Hz; during the approach a target frame every tick, range
 *          floored to whole metres, speed -556 cm/s (20 km/h as ld2451.c
 *          converts it), until the range drops below 2 m (~5.04 s); otherwise
 *          an empty frame every 1.4 s, the bench idle rate
 *   sonar  pings every 60 ms; echoes at 1.5 m for 1 s from the end of the
 *          approach (the cyclist alongside), SONAR_NO_ECHO otherwise
 * Phase runs from each process's start, so senders started together agree. The
 * left radar's pass is offset by $FAKE_PASS_LEFT_OFFSET_S (default half the
 * period). The sonar is on the right side, with the right radar.
 *
 * Exit: 0 (also on SIGTERM or super gone in demo mode), 64 usage, 78 bad socket
 * path or pass setting, 70 other.
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

/* Pass mode. */
#define PASS_ENV         "FAKE_PASS_S"
#define PASS_LEFT_ENV    "FAKE_PASS_LEFT_OFFSET_S"
#define RADAR_MIN_ENV    "FAKE_RADAR_MIN_CM"
#define PASS_START_CM    3000
#define PASS_MIN_CM      200		/* the closest range in the Pi captures */
#define PASS_SPEED_CMS   556		/* 20 km/h: (20 * 1000 + 18) / 36 */
#define PASS_APPROACH_NS ((int64_t)(PASS_START_CM - PASS_MIN_CM) * SEC_NS / PASS_SPEED_CMS)
#define PASS_ALONG_NS    SEC_NS
#define PASS_MIN_S       7		/* approach + alongside + a gap */
#define EMPTY_FRAME_NS   1400000000LL	/* LD2451 with no target (sensor-check.txt) */

struct pass {
	bool    on;
	int64_t period, offset;
};

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

/* $FAKE_RADAR_MIN_CM (100..5000, default 100): the closest the plain radar
 * mode's target comes, e.g. 3100 to keep it outside fusion's 30 m watch range. */
static int radar_min_cm(int *cm)
{
	const char *s = getenv(RADAR_MIN_ENV);
	char *end;
	long v;

	*cm = 100;
	if (!s || !*s)
		return 0;
	errno = 0;
	v = strtol(s, &end, 10);
	if (errno || end == s || *end || v < 100 || v > 5000)
		return -1;
	*cm = (int)v;
	return 0;
}

static void fill_radar(struct radar_msg *m, uint32_t seq, int64_t now, uint8_t side, int min_cm)
{
	int range_cm = 5000 - 50 * (int)(seq % 98);

	memset(m, 0, sizeof(*m));
	m->type = MSG_RADAR;
	m->seq = seq;
	m->origin_ns = (uint64_t)now;
	m->side = side;
	m->ntargets = 1;
	m->t[0].range_cm = (uint16_t)(range_cm > min_cm ? range_cm : min_cm);
	m->t[0].speed_cms = -500;
}

/* $FAKE_PASS_S and, for the left radar, $FAKE_PASS_LEFT_OFFSET_S. */
static int pass_init(struct pass *p, bool left)
{
	const char *s = getenv(PASS_ENV), *o = getenv(PASS_LEFT_ENV);

	memset(p, 0, sizeof(*p));
	if (!s || !*s)
		return 0;
	if (parse_secs(s, &p->period) < 0 || p->period < PASS_MIN_S * SEC_NS)
		return -1;
	if (left) {
		p->offset = p->period / 2;
		if (o && *o && (parse_secs(o, &p->offset) < 0 || p->offset > p->period))
			return -1;
	}
	p->on = true;
	return 0;
}

/* Time into the current pass. Before the first pass (t < offset) the road is
 * empty: a phase past the approach and the cyclist alongside. */
static int64_t pass_phase(const struct pass *p, int64_t start, int64_t now)
{
	int64_t t = now - start - p->offset;

	if (t < 0)
		return PASS_APPROACH_NS + PASS_ALONG_NS;
	return t % p->period;
}

/* A pass-mode radar frame: one approaching target, or empty (ph < 0). */
static void fill_pass_radar(struct radar_msg *m, uint32_t seq, int64_t now, uint8_t side,
			    int64_t ph)
{
	int64_t cm;

	memset(m, 0, sizeof(*m));
	m->type = MSG_RADAR;
	m->seq = seq;
	m->origin_ns = (uint64_t)now;
	m->side = side;
	if (ph < 0)
		return;
	cm = PASS_START_CM - ph / 1000000 * PASS_SPEED_CMS / 1000;
	m->ntargets = 1;
	m->t[0].range_cm = (uint16_t)(cm / 100 * 100);	/* whole metres */
	m->t[0].speed_cms = -PASS_SPEED_CMS;
	m->t[0].dir = 1;				/* approaching */
	m->t[0].snr = 50;
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
	struct pass pass;
	const char *base = strrchr(argv[0], '/') ? strrchr(argv[0], '/') + 1 : argv[0];
	int64_t send_ns = 0, hold_ns = 0, period, start, end, deadline, now, ph;
	int64_t last_empty = 0;		/* valid once have_empty */
	uint64_t sent = 0;
	uint32_t seq;
	bool radar, left, demo, send, have_empty = false;
	int r, min_cm;

	demo = argc == 1 && (strcmp(base, "cap-radar") == 0 || strcmp(base, "cap-radar-left") == 0 ||
			     strcmp(base, "cap-sonar") == 0);
	if (!demo && (argc < 3 || argc > 4 ||
		      (strcmp(argv[1], "radar") && strcmp(argv[1], "radar_left") && strcmp(argv[1], "sonar")) ||
		      parse_secs(argv[2], &send_ns) < 0 ||
		      (argc == 4 && parse_secs(argv[3], &hold_ns) < 0))) {
		log_init("fake-sensors");
		log_msg(LOG_E, "usage args=\"radar|radar_left|sonar <send_s> [hold_s]\"");
		return EX_USAGE;
	}
	left = demo ? strcmp(base, "cap-radar-left") == 0 : strcmp(argv[1], "radar_left") == 0;
	radar = left || (demo ? strcmp(base, "cap-radar") == 0 : strcmp(argv[1], "radar") == 0);
	log_init(demo ? base : left ? "fake-radar-left" : radar ? "fake-radar" : "fake-sonar");
	if (pass_init(&pass, left) < 0) {
		log_msg(LOG_E, "bad_config %s=%s %s=%s want=%d..3600", PASS_ENV,
			getenv(PASS_ENV) ? getenv(PASS_ENV) : "-", PASS_LEFT_ENV,
			getenv(PASS_LEFT_ENV) ? getenv(PASS_LEFT_ENV) : "-", PASS_MIN_S);
		return EX_CONFIG;
	}
	if (radar_min_cm(&min_cm) < 0) {
		log_msg(LOG_E, "bad_config %s=%s want=100..5000", RADAR_MIN_ENV, getenv(RADAR_MIN_ENV));
		return EX_CONFIG;
	}
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
		send = true;
		r = 0;
		if (radar && pass.on) {
			ph = pass_phase(&pass, start, now);
			if (ph < PASS_APPROACH_NS) {
				fill_pass_radar(&rm, seq, now, left ? RADAR_LEFT : RADAR_RIGHT, ph);
			} else if (!have_empty || now - last_empty >= EMPTY_FRAME_NS) {
				fill_pass_radar(&rm, seq, now, left ? RADAR_LEFT : RADAR_RIGHT, -1);
				last_empty = now;
				have_empty = true;
			} else {
				send = false;
			}
			if (send)
				r = ipc_send(&c, &rm, sizeof(rm), now);
		} else if (radar) {
			fill_radar(&rm, seq, now, left ? RADAR_LEFT : RADAR_RIGHT, min_cm);
			r = ipc_send(&c, &rm, sizeof(rm), now);
		} else if (pass.on) {
			ph = pass_phase(&pass, start, now);
			fill_sonar(&sm, seq, now,
				   ph >= PASS_APPROACH_NS && ph < PASS_APPROACH_NS + PASS_ALONG_NS);
			r = ipc_send(&c, &sm, sizeof(sm), now);
		} else {
			fill_sonar(&sm, seq, now, !demo && end - now <= SEC_NS);
			r = ipc_send(&c, &sm, sizeof(sm), now);
		}
		if (send && r == 0)
			sent++;
		else if (send)
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
