/* fuse-hub: receives cap-radar, cap-radar-left and cap-sonar messages (radar
 * side from radar_msg.side), validates them, tracks per-sensor health and runs
 * the fusion state machine (fuse.h; DESIGN.md §1, §3).
 *
 * - Fusion is evaluated on every loop pass (<= POLL_MS apart). When a side's
 *   state or the lost-mask changes, one alert_msg goes to store and to alert;
 *   alert also gets it again every REFRESH_NS, so it can tell a dead hub from
 *   a quiet one. origin_ns is the reading that caused the change, or the hub's
 *   own time when a timer did (hold, track TTL, staleness).
 * - Every valid sensor message is forwarded to store as received.
 * - Both links never block: a missing peer costs messages, counted and
 *   reported on reconnect (store_back, alert_back).
 * - $DLBY_FUSE_RADAR=tracks|presence and $DLBY_FUSE_HOLD_MS (0..10000, default
 *   500) configure fusion.
 * - Heartbeats to dlby-super from the main loop (hb.h); standalone when not
 *   supervised. Event counts every $DLBY_REPORT_S and a final=1 line on exit.
 *
 * - Mechanism C (DESIGN.md §2 C): dlby-super starts it at SCHED_FIFO 70, between
 *   the capture processes (60) and alert (80); it locks its own memory
 *   (mlockall) when supervised.
 *
 * Exit codes: 0 on SIGTERM/SIGINT or when dlby-super is gone, 69 if the socket
 * can't be set up, 78 on a bad setting, 70 on any other unexpected failure.
 */
#include "fuse.h"
#include "hb.h"
#include "health.h"
#include "ipc.h"
#include "lathist.h"
#include "log.h"
#include "msg.h"
#include "report.h"

#include <errno.h>
#include <inttypes.h>
#include <poll.h>
#include <signal.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/signalfd.h>
#include <sysexits.h>
#include <unistd.h>

#define MAX_CLIENTS 8
#define POLL_MS     50
#define REFRESH_NS  500000000LL		/* alert_msg again at 2 Hz (DESIGN.md §1) */
#define HOLD_ENV    "DLBY_FUSE_HOLD_MS"
#define HOLD_MAX_MS 10000

struct client {
	int fd;		/* -1 if the slot is free */
	int sensor;	/* enum sensor of the last valid message, -1 before one */
};

struct hub {
	struct health h;
	struct client cl[MAX_CLIENTS];
	uint64_t rx[SENSOR_N];
	uint64_t rejected;
	struct log_limit lim_bad, lim_full, lim_accept;

	/* Link to store: off the alert path, so sends never block and a missing
	 * store only costs records, counted here and reported on reconnect. */
	struct ipc_conn store;
	uint32_t out_seq;	/* seq of messages the hub itself originates */
	uint64_t store_fwd, store_dropped, store_pending;

	/* Fusion and the link to alert. */
	struct fuse fuse;
	struct alert_msg shown;	/* the last state sent */
	bool     shown_valid;
	int64_t  pass_origin;	/* origin_ns of the newest reading this pass, 0 if none */
	int64_t  next_refresh;
	struct ipc_conn alert;
	uint64_t alerts, alert_dropped, alert_pending;
	uint64_t right_approach, right_in_zone, left_approach;	/* entries */

	struct lathist lat[SENSOR_N];	/* capture -> hub, per sensor */
};

static const char *const lat_name[SENSOR_N] = {
	[SENSOR_RADAR]      = "cap_radar",
	[SENSOR_SONAR]      = "cap_sonar",
	[SENSOR_RADAR_LEFT] = "cap_radar_left",
};

/* One line per histogram, after each counts line. */
static void log_latency(const struct hub *hub, bool final)
{
	char buf[256];
	int s;

	for (s = 0; s < SENSOR_N; s++) {
		lathist_fmt(&hub->lat[s], buf, sizeof(buf));
		log_msg(LOG_I, "latency name=%s %s%s", lat_name[s], buf, final ? " final=1" : "");
	}
}

/* $DLBY_LAT_DIR set: every histogram to a file, for tools/lat-report.py. */
static void dump_latency(const struct hub *hub)
{
	const char *dir = getenv(LATHIST_DIR_ENV);
	int s;

	if (!dir || !*dir)
		return;
	for (s = 0; s < SENSOR_N; s++)
		if (lathist_dump(&hub->lat[s], dir, "fuse-hub", lat_name[s]) < 0)
			log_msg(LOG_W, "lat_dump_failed dir=%s name=%s err=%s", dir, lat_name[s],
				strerror(errno));
}

static void store_send(struct hub *hub, const void *buf, size_t len, int64_t now)
{
	struct gap_msg g;

	if (hub->store_pending) {
		memset(&g, 0, sizeof(g));
		g.type = MSG_GAP;
		g.seq = hub->out_seq++;
		g.origin_ns = (uint64_t)now;
		g.dropped = hub->store_pending > UINT32_MAX ? UINT32_MAX : (uint32_t)hub->store_pending;
		if (ipc_send(&hub->store, &g, sizeof(g), now) < 0)
			goto drop;
		log_msg(LOG_I, "store_back dropped=%" PRIu64, hub->store_pending);
		hub->store_pending = 0;
	}
	if (ipc_send(&hub->store, buf, len, now) == 0) {
		hub->store_fwd++;
		return;
	}
drop:
	hub->store_pending++;
	hub->store_dropped++;
}

/* Same policy as store_send: never block, count drops, say when it's back. */
static void alert_send(struct hub *hub, const struct alert_msg *m, int64_t now)
{
	struct alert_msg out = *m;
	int64_t t = dlby_now_ns();

	/* As late as possible, so hub -> alert latency starts at the send. */
	out.hub_ns = (uint64_t)(t >= 0 ? t : now);
	if (ipc_send(&hub->alert, &out, sizeof(out), now) < 0) {
		hub->alert_pending++;
		hub->alert_dropped++;
		return;
	}
	if (hub->alert_pending) {
		log_msg(LOG_I, "alert_back dropped=%" PRIu64, hub->alert_pending);
		hub->alert_pending = 0;
	}
	hub->alerts++;
}

static unsigned up_mask(const struct health *h)
{
	unsigned up = 0;
	int s;

	for (s = 0; s < SENSOR_N; s++)
		if (h->up[s])
			up |= 1u << s;
	return up;
}

static uint8_t lost_mask(const struct health *h)
{
	return (uint8_t)((h->up[SENSOR_RADAR] ? 0 : LOST_RADAR) |
			 (h->up[SENSOR_SONAR] ? 0 : LOST_SONAR) |
			 (h->up[SENSOR_RADAR_LEFT] ? 0 : LOST_RADAR_LEFT));
}

static void count_entry(struct hub *hub, bool right, enum fuse_state st)
{
	if (right && (st == ST_APPROACHING || st == ST_DEG_APPROACHING))
		hub->right_approach++;
	else if (right && (st == ST_IN_ZONE || st == ST_DEG_IN_ZONE))
		hub->right_in_zone++;
	else if (!right && st == ST_APPROACHING)
		hub->left_approach++;
}

/* Every loop pass: evaluate fusion; on a change tell store and alert, else
 * refresh alert at 2 Hz. */
static void update_fusion(struct hub *hub, int64_t now)
{
	struct fuse_out o;
	struct alert_msg m;
	uint8_t lost = lost_mask(&hub->h);

	fuse_eval(&hub->fuse, up_mask(&hub->h), now, &o);
	if (hub->shown_valid && o.right == hub->shown.state && o.left == hub->shown.state_left &&
	    lost == hub->shown.lost_mask) {
		if (now >= hub->next_refresh) {
			m = hub->shown;
			m.seq = hub->out_seq++;
			alert_send(hub, &m, now);
			hub->next_refresh = now + REFRESH_NS;
		}
		return;
	}
	if (hub->shown_valid && o.right != hub->shown.state) {
		log_msg(LOG_I, "state side=right from=%s to=%s",
			fuse_state_name((enum fuse_state)hub->shown.state), fuse_state_name(o.right));
		count_entry(hub, true, o.right);
	}
	if (hub->shown_valid && o.left != hub->shown.state_left) {
		log_msg(LOG_I, "state side=left from=%s to=%s",
			fuse_state_name((enum fuse_state)hub->shown.state_left), fuse_state_name(o.left));
		count_entry(hub, false, o.left);
	}
	memset(&m, 0, sizeof(m));
	m.type = MSG_ALERT;
	m.state = (uint8_t)o.right;
	m.state_left = (uint8_t)o.left;
	m.lost_mask = lost;
	m.seq = hub->out_seq++;
	/* The reading that caused it; a timer-caused change carries the hub's time. */
	m.origin_ns = (uint64_t)(hub->pass_origin ? hub->pass_origin : now);
	m.hub_ns = (uint64_t)now;	/* alert_send stamps its own copy at the send */
	hub->shown = m;
	hub->shown_valid = true;
	store_send(hub, &m, sizeof(m), now);
	alert_send(hub, &m, now);
	hub->next_refresh = now + REFRESH_NS;
}

static void report_up(struct hub *hub, enum sensor s)
{
	log_msg(LOG_I, "sensor_up name=%s", sensor_name(s));
	if (health_state(&hub->h) == H_OK)
		log_msg(LOG_I, "healthy");
}

static void report_lost(struct hub *hub, unsigned mask, const char *reason)
{
	int s;

	if (!mask)
		return;
	if (health_state(&hub->h) == H_BLIND) {
		log_msg(LOG_E, "blind lost=%s,%s,%s reason=%s", sensor_name(SENSOR_RADAR),
			sensor_name(SENSOR_SONAR), sensor_name(SENSOR_RADAR_LEFT), reason);
	} else {
		for (s = 0; s < SENSOR_N; s++)
			if (mask & (1u << s))
				log_msg(LOG_W, "degraded lost=%s reason=%s", sensor_name(s), reason);
	}
}

/* Another open connection still feeds s (e.g. a restarted cap-* that connected
 * before the old connection's EOF was read). */
static bool fed_elsewhere(const struct hub *hub, int skip, int s)
{
	int i;

	for (i = 0; i < MAX_CLIENTS; i++)
		if (i != skip && hub->cl[i].fd >= 0 && hub->cl[i].sensor == s)
			return true;
	return false;
}

static void drop_client(struct hub *hub, int i, const char *reason)
{
	struct client *c = &hub->cl[i];
	int s = c->sensor;

	/* close() releases the fd on Linux even when it reports an error. */
	if (close(c->fd) < 0)
		log_msg(LOG_W, "close_failed err=%s", strerror(errno));
	c->fd = -1;
	c->sensor = -1;
	if (s >= 0 && !fed_elsewhere(hub, i, s) && health_eof(&hub->h, s))
		report_lost(hub, 1u << s, reason);
}

/* Returns -1 only on an unexpected accept() failure. */
static int accept_client(struct hub *hub, int lfd, int64_t now)
{
	int fd, i;

	fd = ipc_accept(lfd);
	if (fd < 0) {
		if (errno == EAGAIN || errno == EWOULDBLOCK || errno == ECONNABORTED || errno == EINTR)
			return 0;
		if (errno == EMFILE || errno == ENFILE || errno == ENOBUFS || errno == ENOMEM) {
			log_limited(&hub->lim_accept, now, LOG_W, "accept_failed err=%s", strerror(errno));
			return 0;
		}
		log_msg(LOG_E, "accept_failed err=%s", strerror(errno));
		return -1;
	}
	for (i = 0; i < MAX_CLIENTS; i++) {
		if (hub->cl[i].fd < 0) {
			hub->cl[i].fd = fd;
			hub->cl[i].sensor = -1;
			return 0;
		}
	}
	if (close(fd) < 0)
		log_msg(LOG_W, "close_failed err=%s", strerror(errno));
	log_limited(&hub->lim_full, now, LOG_W, "too_many_clients max=%d", MAX_CLIENTS);
	return 0;
}

static void read_client(struct hub *hub, int i, int64_t now)
{
	struct client *c = &hub->cl[i];
	uint8_t buf[MSG_MAX];
	ssize_t n;
	int type, s;

	n = ipc_recv(c->fd, buf, sizeof(buf), &hub->rejected);
	if (n < 0) {
		if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR)
			return;
		if (errno == EMSGSIZE) {
			log_limited(&hub->lim_bad, now, LOG_W, "bad_msg reason=trunc");
			return;
		}
		log_msg(LOG_W, "recv_failed err=%s", strerror(errno));
		drop_client(hub, i, "error");
		return;
	}
	if (n == 0) {
		drop_client(hub, i, "eof");
		return;
	}

	type = ipc_check_msg(buf, (size_t)n, &hub->rejected);
	if (type < 0) {
		log_limited(&hub->lim_bad, now, LOG_W, "bad_msg reason=type len=%zd", n);
		return;
	}
	if (type == MSG_RADAR) {
		struct radar_msg rm;

		memcpy(&rm, buf, sizeof(rm));	/* length checked by ipc_check_msg */
		if (rm.side == RADAR_RIGHT) {
			s = SENSOR_RADAR;
		} else if (rm.side == RADAR_LEFT) {
			s = SENSOR_RADAR_LEFT;
		} else {
			hub->rejected++;
			log_limited(&hub->lim_bad, now, LOG_W, "bad_msg reason=side side=%u",
				    (unsigned)rm.side);
			return;
		}
		fuse_radar(&hub->fuse, s, &rm, now);
		hub->pass_origin = (int64_t)rm.origin_ns;
	} else if (type == MSG_SONAR) {
		struct sonar_msg sm;

		memcpy(&sm, buf, sizeof(sm));	/* length checked by ipc_check_msg */
		s = SENSOR_SONAR;
		fuse_sonar(&hub->fuse, &sm, now);
		hub->pass_origin = (int64_t)sm.origin_ns;
	} else {
		hub->rejected++;
		log_limited(&hub->lim_bad, now, LOG_W, "bad_msg reason=unexpected type=%d", type);
		return;
	}
	c->sensor = s;
	hub->rx[s]++;
	/* capture -> hub. For the sonar this includes up to one 60 ms ping period:
	 * its origin is the kernel's echo-edge stamp, classified at the next ping. */
	lathist_add(&hub->lat[s], now - hub->pass_origin);
	store_send(hub, buf, (size_t)n, now);	/* as received */
	if (health_rx(&hub->h, s, now))
		report_up(hub, s);
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

static void log_counts(const struct hub *hub, const struct report *r, bool final)
{
	static const char *const health[] = { [H_BLIND] = "BLIND", [H_DEG] = "DEG", [H_OK] = "OK" };

	log_msg(LOG_I, "counts n=%" PRIu32 " radar=%" PRIu64 " radar_left=%" PRIu64 " sonar=%" PRIu64
		" rejected=%" PRIu64 " store_fwd=%" PRIu64 " store_dropped=%" PRIu64 " health=%s"
		" fuse=%s right=%s left=%s right_approach=%" PRIu64 " right_in_zone=%" PRIu64
		" left_approach=%" PRIu64 " zone_unconfirmed=%" PRIu64 " alerts=%" PRIu64
		" alert_dropped=%" PRIu64 "%s",
		r->n, hub->rx[SENSOR_RADAR], hub->rx[SENSOR_RADAR_LEFT], hub->rx[SENSOR_SONAR],
		hub->rejected, hub->store_fwd, hub->store_dropped, health[health_state(&hub->h)],
		fuse_mode_name(hub->fuse.mode),
		fuse_state_name((enum fuse_state)hub->shown.state),
		fuse_state_name((enum fuse_state)hub->shown.state_left), hub->right_approach,
		hub->right_in_zone, hub->left_approach, hub->fuse.zone_unconfirmed, hub->alerts,
		hub->alert_dropped, final ? " final=1" : "");
}

/* $DLBY_FUSE_HOLD_MS: 0..HOLD_MAX_MS, default FUSE_HOLD_NS. */
static int parse_hold(int64_t *ns)
{
	const char *s = getenv(HOLD_ENV);
	char *end;
	long v;

	if (!s || !*s) {
		*ns = FUSE_HOLD_NS;
		return 0;
	}
	errno = 0;
	v = strtol(s, &end, 10);
	if (errno || end == s || *end || v < 0 || v > HOLD_MAX_MS) {
		errno = EINVAL;
		return -1;
	}
	*ns = (int64_t)v * 1000000;
	return 0;
}

int main(void)
{
	struct hub hub;
	struct report rep;
	bool rep_ok = false;
	struct hb hb;
	struct pollfd pfd[3 + MAX_CLIENTS];
	int slot[3 + MAX_CLIENTS];
	struct signalfd_siginfo si;
	char path[IPC_PATH_MAX];
	enum fuse_radar_mode mode;
	int64_t now, hold_ns;
	int sfd = -1, lfd = -1, rc = EX_SOFTWARE, supervised;
	int i, n, nfds;
	ssize_t r;

	log_init("fuse-hub");
	memset(&hub, 0, sizeof(hub));
	hub.store.fd = -1;	/* an early goto out must not close fd 0 */
	hub.alert.fd = -1;
	health_init(&hub.h);
	for (i = 0; i < MAX_CLIENTS; i++) {
		hub.cl[i].fd = -1;
		hub.cl[i].sensor = -1;
	}

	supervised = hb_init(&hb);
	if (supervised < 0) {
		log_msg(LOG_E, "hb_init_failed err=%s", strerror(errno));
		goto out;
	}
	/* DESIGN.md §2 C. Unprivileged bench runs can't lock memory: warn, carry on. */
	if (supervised && mlockall(MCL_CURRENT | MCL_FUTURE) < 0)
		log_msg(LOG_W, "mlockall_failed err=%s", strerror(errno));
	if (fuse_parse_mode(getenv(FUSE_RADAR_ENV), &mode) < 0 || parse_hold(&hold_ns) < 0) {
		log_msg(LOG_E, "bad_config %s=%s %s=%s", FUSE_RADAR_ENV,
			getenv(FUSE_RADAR_ENV) ? getenv(FUSE_RADAR_ENV) : "-", HOLD_ENV,
			getenv(HOLD_ENV) ? getenv(HOLD_ENV) : "-");
		rc = EX_CONFIG;
		goto out;
	}
	fuse_init(&hub.fuse, mode, hold_ns);
	sfd = signal_fd();
	if (sfd < 0) {
		log_msg(LOG_E, "signalfd_failed err=%s", strerror(errno));
		goto out;
	}
	if (ipc_path("fuse-hub", path, sizeof(path)) < 0) {
		log_msg(LOG_E, "listen_failed err=%s", strerror(errno));
		rc = EX_UNAVAILABLE;
		goto out;
	}
	lfd = ipc_listen("fuse-hub");
	if (lfd < 0) {
		log_msg(LOG_E, "listen_failed sock=%s err=%s", path, strerror(errno));
		rc = EX_UNAVAILABLE;
		goto out;
	}
	if (ipc_conn_init(&hub.store, "store") < 0) {
		log_msg(LOG_E, "store_path_failed err=%s", strerror(errno));
		rc = EX_UNAVAILABLE;
		goto out;
	}
	if (ipc_conn_init(&hub.alert, "alert") < 0) {
		log_msg(LOG_E, "alert_path_failed err=%s", strerror(errno));
		rc = EX_UNAVAILABLE;
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
	log_msg(LOG_I, "start sock=%s fuse=%s hold_ms=%" PRId64, path, fuse_mode_name(mode),
		hold_ns / 1000000);
	update_fusion(&hub, now);	/* BLIND on both sides */

	for (;;) {
		pfd[0] = (struct pollfd){ .fd = sfd, .events = POLLIN };
		pfd[1] = (struct pollfd){ .fd = lfd, .events = POLLIN };
		pfd[2] = (struct pollfd){ .fd = hb.fd, .events = POLLIN };	/* -1: ignored */
		nfds = 3;
		for (i = 0; i < MAX_CLIENTS; i++) {
			if (hub.cl[i].fd >= 0) {
				slot[nfds] = i;
				pfd[nfds++] = (struct pollfd){ .fd = hub.cl[i].fd, .events = POLLIN };
			}
		}

		n = poll(pfd, (nfds_t)nfds, POLL_MS);
		if (n < 0) {
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
		hub.pass_origin = 0;

		if (pfd[0].revents) {
			r = read(sfd, &si, sizeof(si));
			if (r == (ssize_t)sizeof(si)) {
				rc = EX_OK;
				goto out;
			}
			if (r < 0 && (errno == EINTR || errno == EAGAIN))
				continue;
			log_msg(LOG_E, "signalfd_read_failed err=%s", r < 0 ? strerror(errno) : "short");
			goto out;
		}
		if ((pfd[1].revents & POLLIN) && accept_client(&hub, lfd, now) < 0)
			goto out;
		for (i = 3; i < nfds; i++)
			if (pfd[i].revents)
				read_client(&hub, slot[i], now);

		report_lost(&hub, health_tick(&hub.h, now), "stale");
		update_fusion(&hub, now);	/* every pass: holds and TTLs run on time */

		/* From this loop, never a side thread: a stuck loop goes quiet. */
		if ((pfd[2].revents && hb_readable(&hb) < 0) || hb_tick(&hb, now) < 0) {
			log_msg(LOG_W, "super_lost err=%s", strerror(errno));
			rc = EX_OK;
			goto out;
		}
		if (report_due(&rep, now)) {
			log_counts(&hub, &rep, false);
			log_latency(&hub, false);
		}
	}

out:
	for (i = 0; i < MAX_CLIENTS; i++)
		if (hub.cl[i].fd >= 0 && close(hub.cl[i].fd) < 0)
			log_msg(LOG_W, "close_failed err=%s", strerror(errno));
	if (lfd >= 0 && close(lfd) < 0)
		log_msg(LOG_W, "close_failed err=%s", strerror(errno));
	if (sfd >= 0 && close(sfd) < 0)
		log_msg(LOG_W, "close_failed err=%s", strerror(errno));
	ipc_conn_close(&hub.store);
	ipc_conn_close(&hub.alert);
	if (rep_ok) {
		report_final(&rep);
		log_counts(&hub, &rep, true);
		log_latency(&hub, true);
		dump_latency(&hub);
	}
	log_msg(LOG_I, "store_link forwarded=%" PRIu64 " dropped=%" PRIu64, hub.store_fwd,
		hub.store_dropped);
	log_msg(rc == EX_OK ? LOG_I : LOG_E, "exit code=%d radar=%" PRIu64 " radar_left=%" PRIu64
		" sonar=%" PRIu64 " rejected=%" PRIu64, rc, hub.rx[SENSOR_RADAR],
		hub.rx[SENSOR_RADAR_LEFT], hub.rx[SENSOR_SONAR], hub.rejected);
	return rc;
}
