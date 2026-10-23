/* store (M3 step 3): power-cut-safe append log of everything fuse-hub forwards
 * (DESIGN.md §2 D, §4). Off the alert path: runs SCHED_OTHER.
 *
 * Not yet: the heartbeat to dlby-super (it goes in the main loop below) and
 * nice +5, both with the super step.
 *
 * Exit codes: 0 on SIGTERM/SIGINT, 69 if the log dir or socket can't be set up,
 * 70 on any other unexpected failure (restart runs recovery).
 */
#include "ipc.h"
#include "log.h"
#include "msg.h"
#include "seglog.h"

#include <errno.h>
#include <inttypes.h>
#include <poll.h>
#include <signal.h>
#include <string.h>
#include <sys/signalfd.h>
#include <sysexits.h>
#include <unistd.h>

#define MAX_CLIENTS 4
#define MAX_IDLE_MS 1000

struct store {
	struct seglog sl;
	int cl[MAX_CLIENTS];	/* -1 if free */
	uint64_t rejected;
	struct log_limit lim_bad, lim_full, lim_accept, lim_nospc;
};

static int signal_fd(void)
{
	sigset_t mask;

	if (sigemptyset(&mask) < 0 || sigaddset(&mask, SIGTERM) < 0 || sigaddset(&mask, SIGINT) < 0)
		return -1;
	if (sigprocmask(SIG_BLOCK, &mask, NULL) < 0)
		return -1;
	return signalfd(-1, &mask, SFD_CLOEXEC);
}

static void close_client(struct store *st, int i)
{
	/* close() releases the fd on Linux even when it reports an error. */
	if (close(st->cl[i]) < 0)
		log_msg(LOG_W, "close_failed err=%s", strerror(errno));
	st->cl[i] = -1;
}

/* Returns -1 only on an unexpected accept() failure. */
static int accept_client(struct store *st, int lfd, int64_t now)
{
	int fd, i;

	fd = ipc_accept(lfd);
	if (fd < 0) {
		if (errno == EAGAIN || errno == EWOULDBLOCK || errno == ECONNABORTED || errno == EINTR)
			return 0;
		if (errno == EMFILE || errno == ENFILE || errno == ENOBUFS || errno == ENOMEM) {
			log_limited(&st->lim_accept, now, LOG_W, "accept_failed err=%s", strerror(errno));
			return 0;
		}
		log_msg(LOG_E, "accept_failed err=%s", strerror(errno));
		return -1;
	}
	for (i = 0; i < MAX_CLIENTS; i++) {
		if (st->cl[i] < 0) {
			st->cl[i] = fd;
			return 0;
		}
	}
	if (close(fd) < 0)
		log_msg(LOG_W, "close_failed err=%s", strerror(errno));
	log_limited(&st->lim_full, now, LOG_W, "too_many_clients max=%d", MAX_CLIENTS);
	return 0;
}

/* Returns -1 only on a fatal log write error. */
static int read_client(struct store *st, int i, int64_t now)
{
	uint8_t buf[MSG_MAX];
	ssize_t n;
	int type;

	n = ipc_recv(st->cl[i], buf, sizeof(buf), &st->rejected);
	if (n < 0) {
		if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR)
			return 0;
		if (errno == EMSGSIZE) {
			log_limited(&st->lim_bad, now, LOG_W, "bad_msg reason=trunc");
			return 0;
		}
		log_msg(LOG_W, "recv_failed err=%s", strerror(errno));
		close_client(st, i);
		return 0;
	}
	if (n == 0) {
		close_client(st, i);
		return 0;
	}
	type = ipc_check_msg(buf, (size_t)n, &st->rejected);
	if (type < 0) {
		log_limited(&st->lim_bad, now, LOG_W, "bad_msg reason=type len=%zd", n);
		return 0;
	}
	if (type == MSG_HB) {
		st->rejected++;
		log_limited(&st->lim_bad, now, LOG_W, "bad_msg reason=unexpected type=%d", type);
		return 0;
	}

	/* Stored exactly as received; the record type is the message's type byte. */
	if (seglog_append(&st->sl, (uint8_t)type, buf, (size_t)n, now) < 0) {
		if (errno == ENOSPC) {
			log_limited(&st->lim_nospc, now, LOG_E, "write_failed err=%s type=%d",
				    strerror(errno), type);
			return 0;
		}
		log_msg(LOG_E, "write_failed err=%s", strerror(errno));
		return -1;
	}
	return 0;
}

/* Idle at most until the next timed sync is due, never more than MAX_IDLE_MS. */
static int poll_timeout_ms(const struct seglog *sl, int64_t now)
{
	int64_t due = seglog_sync_due_ns(sl), ms;

	if (due == INT64_MAX)
		return MAX_IDLE_MS;
	ms = (due - now + 999999) / 1000000;
	return ms < 0 ? 0 : ms > MAX_IDLE_MS ? MAX_IDLE_MS : (int)ms;
}

int main(void)
{
	struct store st;
	struct seglog_cfg cfg;
	struct pollfd pfd[2 + MAX_CLIENTS];
	int slot[2 + MAX_CLIENTS];
	struct signalfd_siginfo si;
	bool sl_open = false;
	int64_t now;
	int sfd = -1, lfd = -1, rc = EX_SOFTWARE;
	int i, n, nfds;
	ssize_t r;

	log_init("store");
	memset(&st, 0, sizeof(st));
	for (i = 0; i < MAX_CLIENTS; i++)
		st.cl[i] = -1;

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
	seglog_default_cfg(&cfg);
	if (seglog_open(&st.sl, &cfg, now) < 0) {
		log_msg(LOG_E, "log_open_failed dir=%s err=%s", cfg.dir, strerror(errno));
		rc = EX_UNAVAILABLE;
		goto out;
	}
	sl_open = true;
	lfd = ipc_listen("store");
	if (lfd < 0) {
		log_msg(LOG_E, "listen_failed err=%s", strerror(errno));
		rc = EX_UNAVAILABLE;
		goto out;
	}
	log_msg(LOG_I, "start dir=%s segment=%s next_seq=%" PRIu32, st.sl.dir, st.sl.seg_name,
		st.sl.next_seq);

	for (;;) {
		pfd[0] = (struct pollfd){ .fd = sfd, .events = POLLIN };
		pfd[1] = (struct pollfd){ .fd = lfd, .events = POLLIN };
		nfds = 2;
		for (i = 0; i < MAX_CLIENTS; i++) {
			if (st.cl[i] >= 0) {
				slot[nfds] = i;
				pfd[nfds++] = (struct pollfd){ .fd = st.cl[i], .events = POLLIN };
			}
		}

		n = poll(pfd, (nfds_t)nfds, poll_timeout_ms(&st.sl, now));
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
			if ((pfd[1].revents & POLLIN) && accept_client(&st, lfd, now) < 0)
				goto out;
			for (i = 2; i < nfds; i++)
				if (pfd[i].revents && read_client(&st, slot[i], now) < 0)
					goto out;
		}

		/* Every pass, not just on timeout: a steady stream never lets poll
		 * time out, and samples must still reach disk within ~1 s. */
		/* TODO(super step): heartbeat to dlby-super here, from this loop. */
		if (seglog_maybe_sync(&st.sl, now) < 0) {
			log_msg(LOG_E, "sync_failed err=%s", strerror(errno));
			goto out;
		}
	}

out:
	for (i = 0; i < MAX_CLIENTS; i++)
		if (st.cl[i] >= 0)
			close_client(&st, i);
	if (lfd >= 0 && close(lfd) < 0)
		log_msg(LOG_W, "close_failed err=%s", strerror(errno));
	if (sfd >= 0 && close(sfd) < 0)
		log_msg(LOG_W, "close_failed err=%s", strerror(errno));
	if (sl_open && seglog_close(&st.sl) < 0) {
		log_msg(LOG_E, "close_log_failed err=%s", strerror(errno));
		rc = EX_SOFTWARE;
	}
	log_msg(rc == EX_OK ? LOG_I : LOG_E,
		"exit code=%d records=%" PRIu64 " syncs_timed=%" PRIu64 " syncs_state=%" PRIu64, rc,
		st.sl.records, st.sl.syncs_timed, st.sl.syncs_state);
	return rc;
}
