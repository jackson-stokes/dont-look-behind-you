/* Stand-in for fuse-hub's alert link, for testing alert without the hub. Never
 * deployed.
 *
 *   send-alert <step>...
 *
 *   RIGHT:LEFT:MS   send this state pair (names as in DESIGN.md §1, e.g.
 *                   IN_ZONE:CLEAR:1000) at once and then every 500 ms, like the
 *                   hub's 2 Hz refresh, for MS ms
 *   hold:MS         keep the connection open and send nothing for MS ms, so
 *                   only alert's own silence timeout can notice
 *   bad:MS          send one message with an invalid state, then wait MS ms
 *
 * Connects to $DLBY_RUN_DIR/alert.sock, retrying for up to 3 s. Exits when the
 * steps are done, closing the connection.
 *
 * Exit: 0, 64 usage, 70 if alert never accepted the first message.
 */
#include "ipc.h"
#include "log.h"
#include "msg.h"
#include "pattern.h"

#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <sysexits.h>
#include <time.h>

#define MS          1000000LL
#define REFRESH_NS  (500 * MS)
#define CONNECT_NS  (3000 * MS)

struct step {
	enum { STEP_SEND, STEP_HOLD, STEP_BAD } kind;
	unsigned right, left;
	int64_t  ns;
};

static int parse_state(const char *s, size_t len, unsigned *out)
{
	unsigned i;

	for (i = 0; i < PAT_HUB_LOST; i++) {	/* hub states only, not alert's own */
		if (strlen(pattern_state_name(i)) == len && strncmp(s, pattern_state_name(i), len) == 0) {
			*out = i;
			return 0;
		}
	}
	return -1;
}

static int parse_ms(const char *s, int64_t *ns)
{
	char *end;
	long v;

	errno = 0;
	v = strtol(s, &end, 10);
	if (errno || end == s || *end || v < 0 || v > 600000)
		return -1;
	*ns = v * MS;
	return 0;
}

static int parse_step(const char *arg, struct step *st)
{
	const char *c1 = strchr(arg, ':'), *c2;

	if (!c1)
		return -1;
	if (strncmp(arg, "hold:", 5) == 0) {
		st->kind = STEP_HOLD;
		return parse_ms(arg + 5, &st->ns);
	}
	if (strncmp(arg, "bad:", 4) == 0) {
		st->kind = STEP_BAD;
		return parse_ms(arg + 4, &st->ns);
	}
	c2 = strchr(c1 + 1, ':');
	if (!c2)
		return -1;
	st->kind = STEP_SEND;
	if (parse_state(arg, (size_t)(c1 - arg), &st->right) < 0 ||
	    parse_state(c1 + 1, (size_t)(c2 - c1 - 1), &st->left) < 0)
		return -1;
	return parse_ms(c2 + 1, &st->ns);
}

static void sleep_until(int64_t t)
{
	struct timespec ts = { .tv_sec = t / 1000000000, .tv_nsec = t % 1000000000 };

	while (clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &ts, NULL) == EINTR)
		;
}

static int send_pair(struct ipc_conn *c, uint32_t *seq, unsigned right, unsigned left)
{
	struct alert_msg m;
	int64_t now = dlby_now_ns();

	memset(&m, 0, sizeof(m));
	m.type = MSG_ALERT;
	m.state = (uint8_t)right;
	m.state_left = (uint8_t)left;
	m.seq = (*seq)++;
	m.origin_ns = (uint64_t)now;
	return ipc_send(c, &m, sizeof(m), now);
}

int main(int argc, char **argv)
{
	struct step st;
	struct ipc_conn c;
	uint32_t seq = 0;
	int64_t t, end, give_up;
	bool connected = false;
	int i;

	log_init("send-alert");
	if (argc < 2) {
		log_msg(LOG_E, "usage steps=RIGHT:LEFT:MS|hold:MS|bad:MS...");
		return EX_USAGE;
	}
	for (i = 1; i < argc; i++) {
		if (parse_step(argv[i], &st) < 0) {
			log_msg(LOG_E, "bad_step %s", argv[i]);
			return EX_USAGE;
		}
	}
	if (ipc_conn_init(&c, "alert") < 0) {
		log_msg(LOG_E, "bad_sock_path err=%s", strerror(errno));
		return EX_SOFTWARE;
	}

	for (i = 1; i < argc; i++) {
		parse_step(argv[i], &st);
		t = dlby_now_ns();
		end = t + st.ns;
		if (st.kind == STEP_BAD) {
			if (send_pair(&c, &seq, 99, ST_CLEAR) < 0)
				log_msg(LOG_W, "send_failed err=%s", strerror(errno));
		} else if (st.kind == STEP_SEND) {
			/* The first message waits out alert's start; later ones may drop. */
			give_up = t + CONNECT_NS;
			while (!connected && send_pair(&c, &seq, st.right, st.left) < 0) {
				if (dlby_now_ns() > give_up) {
					log_msg(LOG_E, "connect_failed err=%s", strerror(errno));
					ipc_conn_close(&c);
					return EX_SOFTWARE;
				}
				sleep_until(dlby_now_ns() + 20 * MS);
			}
			if (connected && send_pair(&c, &seq, st.right, st.left) < 0)
				log_msg(LOG_W, "send_failed err=%s", strerror(errno));
			connected = true;
			for (t += REFRESH_NS; t < end; t += REFRESH_NS) {
				sleep_until(t);
				if (send_pair(&c, &seq, st.right, st.left) < 0)
					log_msg(LOG_W, "send_failed err=%s", strerror(errno));
			}
		}
		sleep_until(end);
	}
	ipc_conn_close(&c);
	log_msg(LOG_I, "done sent=%u", (unsigned)seq);
	return EX_OK;
}
