/* Fake supervised child for testing dlby-super. Never deployed.
 *
 * Run under a child's name (a symlink: alert, status-lcd, cap-radar, ...). Its
 * behaviour comes from $FAKE_<name>, with '-' as '_' (e.g. FAKE_status_lcd):
 *
 *   ok (default)     heartbeat every 1 s until SIGTERM, then exit 0
 *   crash_once:<s>   after s seconds die by SIGSEGV (under ASan, the caller sets
 *                    ASAN_OPTIONS=handle_segv=0 so this is a real signal death)
 *   hang_once:<s>    after s seconds stop heartbeating and ignore SIGTERM
 *   exit:<code>      exit with code at once
 *
 * "_once" modes misbehave only on the first run: a marker file in $FAKE_STATE_DIR
 * records that it happened, so the restarted child runs "ok". At start it logs
 * the fds it inherited, "I fds open=0,1,2,3".
 */
#include "hb.h"
#include "log.h"
#include "msg.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sysexits.h>
#include <time.h>
#include <unistd.h>

#define TICK_NS (100LL * 1000000LL)

static volatile sig_atomic_t stop;

static void on_term(int sig)
{
	(void)sig;
	stop = 1;
}

/* Log the open fds, not counting the one reading /proc/self/fd. */
static int log_fds(void)
{
	char list[256] = "";
	struct dirent *de;
	DIR *d = opendir("/proc/self/fd");
	int self, n = 0;

	if (!d)
		return -1;
	self = dirfd(d);
	for (;;) {
		errno = 0;
		de = readdir(d);
		if (!de)
			break;
		if (de->d_name[0] == '.' || atoi(de->d_name) == self)
			continue;
		if (n++)
			strncat(list, ",", sizeof(list) - strlen(list) - 1);
		strncat(list, de->d_name, sizeof(list) - strlen(list) - 1);
	}
	if (errno) {
		(void)closedir(d);
		return -1;
	}
	if (closedir(d) < 0)
		return -1;
	log_msg(LOG_I, "fds open=%s", list);
	return 0;
}

/* True the first time only: creates the marker file. */
static bool first_time(const char *name)
{
	const char *dir = getenv("FAKE_STATE_DIR");
	char path[256];
	int fd;

	if (!dir || !*dir)
		return true;
	snprintf(path, sizeof(path), "%s/%s.done", dir, name);
	fd = open(path, O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0600);
	if (fd < 0)
		return false;
	(void)close(fd);
	return true;
}

int main(int argc, char **argv)
{
	const char *name = strrchr(argv[0], '/') ? strrchr(argv[0], '/') + 1 : argv[0];
	char var[64], *p;
	const char *mode;
	struct timespec ts = { .tv_sec = 0, .tv_nsec = TICK_NS };
	struct sigaction sa;
	struct hb hb;
	int64_t start, now, at_ns = 0;
	bool crash = false, hang = false, hanging = false;
	double secs = 0;

	(void)argc;
	log_init(name);
	if (log_fds() < 0)
		log_msg(LOG_W, "fds_unreadable err=%s", strerror(errno));
	if (hb_init(&hb) < 0) {
		log_msg(LOG_E, "hb_init_failed err=%s", strerror(errno));
		return EX_SOFTWARE;
	}
	snprintf(var, sizeof(var), "FAKE_%s", name);
	for (p = var; *p; p++)
		if (*p == '-')
			*p = '_';
	mode = getenv(var);
	if (!mode || !*mode)
		mode = "ok";

	if (strncmp(mode, "exit:", 5) == 0) {
		log_msg(LOG_I, "exiting code=%d", atoi(mode + 5));
		return atoi(mode + 5);
	}
	if (sscanf(mode, "crash_once:%lf", &secs) == 1) {
		crash = first_time(name);
	} else if (sscanf(mode, "hang_once:%lf", &secs) == 1) {
		hang = first_time(name);
	} else if (strcmp(mode, "ok") != 0) {
		log_msg(LOG_E, "bad_mode %s=%s", var, mode);
		return EX_CONFIG;
	}

	memset(&sa, 0, sizeof(sa));
	sa.sa_handler = on_term;
	if (sigemptyset(&sa.sa_mask) < 0 || sigaction(SIGTERM, &sa, NULL) < 0) {
		log_msg(LOG_E, "sigaction_failed err=%s", strerror(errno));
		return EX_SOFTWARE;
	}
	start = dlby_now_ns();
	if (start < 0)
		return EX_SOFTWARE;
	if (crash || hang)
		at_ns = start + (int64_t)(secs * 1e9);

	while (!stop || hanging) {
		now = dlby_now_ns();
		if (now < 0)
			return EX_SOFTWARE;
		if (crash && now >= at_ns) {
			log_msg(LOG_I, "crashing signal=SEGV");
			raise(SIGSEGV);
		}
		if (hang && now >= at_ns && !hanging) {
			log_msg(LOG_I, "hanging");
			if (signal(SIGTERM, SIG_IGN) == SIG_ERR)
				return EX_SOFTWARE;
			hanging = true;
		}
		if (!hanging && (hb_readable(&hb) < 0 || hb_tick(&hb, now) < 0)) {
			log_msg(LOG_W, "super_lost err=%s", strerror(errno));
			return EX_OK;
		}
		if (nanosleep(&ts, NULL) < 0 && errno != EINTR)
			return EX_SOFTWARE;
	}
	log_msg(LOG_I, "exit code=0");
	return EX_OK;
}
