/* cap-radar: reads LD2451 frames from the radar's UART and sends one
 * radar_msg per valid frame to fuse-hub (DESIGN.md §1). Heartbeats to
 * dlby-super from the main loop (hb.h); standalone when not supervised.
 *
 * One binary, two radars: run as cap-radar it is the right radar, run as
 * cap-radar-left (a symlink) the left one. The side goes in every message.
 *
 *   $DLBY_RADAR_DEV       right radar (default /dev/ttyAMA2: UART2, GPIO 4/5)
 *   $DLBY_RADAR_LEFT_DEV  left radar (default /dev/ttyAMA4: UART4, GPIO 12/13)
 *
 * Every message has ntargets=0 until a real target capture verifies the
 * LD2451 target layout; frames with a payload are counted and logged in hex
 * (rate limited) so they can be decoded. origin_ns is the user-space read time
 * of the frame's last byte.
 *
 * Exit codes: 0 on SIGTERM/SIGINT or when dlby-super is gone, 78 if the device
 * is missing or not a tty (restarting won't fix it), 69 if it errors or hangs
 * up, 70 on any other unexpected failure.
 */
#include "hb.h"
#include "health.h"
#include "ipc.h"
#include "log.h"
#include "msg.h"
#include "radar_parse.h"

#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <poll.h>
#include <signal.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/signalfd.h>
#include <sysexits.h>
#include <termios.h>
#include <unistd.h>

#define LEFT_NAME   "cap-radar-left"
#define POLL_MS     100

struct radar {
	uint8_t side;		/* enum radar_side */
	struct radar_parser p;
	struct ipc_conn hub;
	uint32_t seq;
	uint64_t sent, target_frames;
	int64_t last_frame_ns;	/* -1 before the first frame */
	bool silent;
	struct log_limit lim_len, lim_tail, lim_target, lim_send;
};

/* Raw 115200 8N1, no flow control, reads never block. */
static int tty_setup(int fd)
{
	struct termios t;

	if (tcgetattr(fd, &t) < 0)
		return -1;
	cfmakeraw(&t);
	t.c_cflag |= CLOCAL | CREAD;
	t.c_cflag &= ~(tcflag_t)(CSTOPB | CRTSCTS);
	t.c_cc[VMIN] = 0;
	t.c_cc[VTIME] = 0;
	if (cfsetispeed(&t, B115200) < 0 || cfsetospeed(&t, B115200) < 0)
		return -1;
	if (tcsetattr(fd, TCSANOW, &t) < 0)
		return -1;
	return tcflush(fd, TCIFLUSH);	/* drop any half frame from before we started */
}

static void log_target_frame(struct radar *r, int64_t now)
{
	char hex[2 * RADAR_FRAME_MAX + 1];
	size_t i, total = RADAR_FRAME_OVERHEAD + r->p.len;

	for (i = 0; i < total; i++) {
		static const char digits[] = "0123456789abcdef";

		hex[2 * i] = digits[r->p.buf[i] >> 4];
		hex[2 * i + 1] = digits[r->p.buf[i] & 0xf];
	}
	hex[2 * total] = '\0';
	log_limited(&r->lim_target, now, LOG_I, "target_frame len=%zu hex=%s", r->p.len, hex);
}

static void on_frame(struct radar *r, int64_t now)
{
	struct radar_msg m;

	if (r->last_frame_ns < 0)
		log_msg(LOG_I, "radar_up");
	else if (r->silent)
		log_msg(LOG_I, "radar_back silent_ms=%" PRId64, (now - r->last_frame_ns) / 1000000);
	r->silent = false;
	r->last_frame_ns = now;

	if (r->p.len > 0) {
		r->target_frames++;
		log_target_frame(r, now);
	}

	memset(&m, 0, sizeof(m));
	m.type = MSG_RADAR;
	m.ntargets = 0;		/* target layout unverified: see the header comment */
	m.side = r->side;
	m.seq = r->seq++;
	m.origin_ns = (uint64_t)now;
	if (ipc_send(&r->hub, &m, sizeof(m), now) == 0)
		r->sent++;
	else
		log_limited(&r->lim_send, now, LOG_W, "send_dropped err=%s", strerror(errno));
}

static void feed(struct radar *r, const uint8_t *buf, size_t n, int64_t now)
{
	size_t i;

	for (i = 0; i < n; i++) {
		switch (radar_feed(&r->p, buf[i])) {
		case RADAR_FRAME:
			on_frame(r, now);
			break;
		case RADAR_BAD_LEN:
			log_limited(&r->lim_len, now, LOG_W, "bad_frame reason=len");
			break;
		case RADAR_BAD_TAIL:
			log_limited(&r->lim_tail, now, LOG_W, "bad_frame reason=tail");
			break;
		case RADAR_MORE:
			break;
		}
	}
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

int main(int argc, char **argv)
{
	const char *base = strrchr(argv[0], '/') ? strrchr(argv[0], '/') + 1 : argv[0];
	bool left = strcmp(base, LEFT_NAME) == 0;
	struct radar r;
	struct hb hb;
	struct pollfd pfd[3];
	struct signalfd_siginfo si;
	uint8_t buf[256];
	const char *dev = getenv(left ? "DLBY_RADAR_LEFT_DEV" : "DLBY_RADAR_DEV");
	int64_t now, start;
	int sfd = -1, tty = -1, rc = EX_SOFTWARE, supervised;
	ssize_t n;

	(void)argc;
	log_init(left ? LEFT_NAME : "cap-radar");
	memset(&r, 0, sizeof(r));
	r.side = left ? RADAR_LEFT : RADAR_RIGHT;
	r.hub.fd = -1;		/* an early goto out must not close fd 0 */
	r.last_frame_ns = -1;
	radar_parser_init(&r.p);
	if (!dev || !*dev)
		dev = left ? "/dev/ttyAMA4" : "/dev/ttyAMA2";

	supervised = hb_init(&hb);
	if (supervised < 0) {
		log_msg(LOG_E, "hb_init_failed err=%s", strerror(errno));
		goto out;
	}
	/* DESIGN.md §2 C. Unprivileged bench runs can't lock memory: warn, carry on. */
	if (supervised && mlockall(MCL_CURRENT | MCL_FUTURE) < 0)
		log_msg(LOG_W, "mlockall_failed err=%s", strerror(errno));
	sfd = signal_fd();
	if (sfd < 0) {
		log_msg(LOG_E, "signalfd_failed err=%s", strerror(errno));
		goto out;
	}
	if (ipc_conn_init(&r.hub, "fuse-hub") < 0) {
		log_msg(LOG_E, "bad_sock_path err=%s", strerror(errno));
		rc = EX_CONFIG;
		goto out;
	}
	tty = open(dev, O_RDWR | O_NOCTTY | O_NONBLOCK | O_CLOEXEC);
	if (tty < 0) {
		log_msg(LOG_E, "open_failed dev=%s err=%s", dev, strerror(errno));
		rc = errno == ENOENT || errno == EACCES || errno == ENOTDIR ? EX_CONFIG : EX_UNAVAILABLE;
		goto out;
	}
	if (tty_setup(tty) < 0) {
		log_msg(LOG_E, "tty_setup_failed dev=%s err=%s", dev, strerror(errno));
		rc = errno == ENOTTY ? EX_CONFIG : EX_UNAVAILABLE;
		goto out;
	}
	start = dlby_now_ns();
	if (start < 0) {
		log_msg(LOG_E, "clock_failed err=%s", strerror(errno));
		goto out;
	}
	log_msg(LOG_I, "start dev=%s", dev);

	for (;;) {
		pfd[0] = (struct pollfd){ .fd = sfd, .events = POLLIN };
		pfd[1] = (struct pollfd){ .fd = tty, .events = POLLIN };
		pfd[2] = (struct pollfd){ .fd = hb.fd, .events = POLLIN };	/* -1: ignored */

		if (poll(pfd, 3, POLL_MS) < 0) {
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
		if (pfd[1].revents & POLLIN) {
			n = read(tty, buf, sizeof(buf));
			if (n > 0) {
				feed(&r, buf, (size_t)n, now);
			} else if (n < 0 && errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR) {
				log_msg(LOG_E, "read_failed dev=%s err=%s", dev, strerror(errno));
				rc = EX_UNAVAILABLE;
				goto out;
			}
		}
		if (pfd[1].revents & (POLLHUP | POLLERR | POLLNVAL) && !(pfd[1].revents & POLLIN)) {
			log_msg(LOG_E, "hangup dev=%s revents=0x%x", dev, (unsigned)pfd[1].revents);
			rc = EX_UNAVAILABLE;
			goto out;
		}

		/* fuse-hub declares the radar lost on its own; this line is for the bench. */
		if (!r.silent && now - (r.last_frame_ns < 0 ? start : r.last_frame_ns) > RADAR_LOSS_NS) {
			log_msg(LOG_W, "radar_silent dev=%s", dev);
			r.silent = true;
		}

		/* From this loop, never a side thread: a stuck loop goes quiet. */
		if ((pfd[2].revents && hb_readable(&hb) < 0) || hb_tick(&hb, now) < 0) {
			log_msg(LOG_W, "super_lost err=%s", strerror(errno));
			rc = EX_OK;
			goto out;
		}
	}

out:
	if (tty >= 0 && close(tty) < 0)
		log_msg(LOG_W, "close_failed err=%s", strerror(errno));
	if (sfd >= 0 && close(sfd) < 0)
		log_msg(LOG_W, "close_failed err=%s", strerror(errno));
	ipc_conn_close(&r.hub);
	log_msg(rc == EX_OK ? LOG_I : LOG_E, "exit code=%d frames=%" PRIu64 " target_frames=%" PRIu64
		" bad_len=%" PRIu64 " bad_tail=%" PRIu64 " sent=%" PRIu64 " dropped=%" PRIu64,
		rc, r.p.frames, r.target_frames, r.p.bad_len, r.p.bad_tail, r.sent, r.hub.dropped);
	return rc;
}
