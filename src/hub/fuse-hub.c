/* fuse-hub, minimal (M3 step 2): receives cap-radar/cap-sonar messages, validates
 * them and tracks per-sensor health (DESIGN.md §1, §3).
 *
 * Not yet: APPROACHING/IN_ZONE fusion, forwarding to alert and store, the
 * heartbeat to dlby-super (it goes in the main loop below), SCHED_FIFO 70 and
 * mlockall.
 *
 * Exit codes: 0 on SIGTERM/SIGINT, 69 if the socket can't be set up, 70 on any
 * other unexpected failure.
 */
#include "health.h"
#include "ipc.h"
#include "log.h"
#include "msg.h"

#include <errno.h>
#include <inttypes.h>
#include <poll.h>
#include <signal.h>
#include <string.h>
#include <sys/signalfd.h>
#include <sysexits.h>
#include <unistd.h>

#define MAX_CLIENTS 8
#define POLL_MS     50
#define STATS_NS    (10LL * 1000000000LL)

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
};

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
		log_msg(LOG_E, "blind lost=radar,sonar reason=%s", reason);
		return;
	}
	for (s = 0; s < SENSOR_N; s++)
		if (mask & (1u << s))
			log_msg(LOG_W, "degraded lost=%s reason=%s", sensor_name(s), reason);
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
		s = SENSOR_RADAR;
	} else if (type == MSG_SONAR) {
		s = SENSOR_SONAR;
	} else {
		hub->rejected++;
		log_limited(&hub->lim_bad, now, LOG_W, "bad_msg reason=unexpected type=%d", type);
		return;
	}
	c->sensor = s;
	hub->rx[s]++;
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

int main(void)
{
	struct hub hub;
	struct pollfd pfd[2 + MAX_CLIENTS];
	int slot[2 + MAX_CLIENTS];
	struct signalfd_siginfo si;
	char path[IPC_PATH_MAX];
	int64_t now, next_stats;
	int sfd = -1, lfd = -1, rc = EX_SOFTWARE;
	int i, n, nfds;
	ssize_t r;

	log_init("fuse-hub");
	memset(&hub, 0, sizeof(hub));
	health_init(&hub.h);
	for (i = 0; i < MAX_CLIENTS; i++) {
		hub.cl[i].fd = -1;
		hub.cl[i].sensor = -1;
	}

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
	now = dlby_now_ns();
	if (now < 0) {
		log_msg(LOG_E, "clock_failed err=%s", strerror(errno));
		goto out;
	}
	next_stats = now + STATS_NS;
	log_msg(LOG_I, "start sock=%s", path);

	for (;;) {
		pfd[0] = (struct pollfd){ .fd = sfd, .events = POLLIN };
		pfd[1] = (struct pollfd){ .fd = lfd, .events = POLLIN };
		nfds = 2;
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
		for (i = 2; i < nfds; i++)
			if (pfd[i].revents)
				read_client(&hub, slot[i], now);

		report_lost(&hub, health_tick(&hub.h, now), "stale");

		/* TODO(super step): heartbeat to dlby-super here, from this loop. */
		if (now >= next_stats) {
			log_msg(LOG_I, "stats radar=%" PRIu64 " sonar=%" PRIu64 " rejected=%" PRIu64,
				hub.rx[SENSOR_RADAR], hub.rx[SENSOR_SONAR], hub.rejected);
			next_stats = now + STATS_NS;
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
	log_msg(rc == EX_OK ? LOG_I : LOG_E, "exit code=%d radar=%" PRIu64 " sonar=%" PRIu64
		" rejected=%" PRIu64, rc, hub.rx[SENSOR_RADAR], hub.rx[SENSOR_SONAR], hub.rejected);
	return rc;
}
