/* dlby-super (mechanism E): starts, watches and restarts every dlby process
 * (DESIGN.md §2 E, §3).
 *
 * - Starts the children in DESIGN order. Each inherits only fd 3, its end of a
 *   heartbeat socketpair; every other fd is close-on-exec.
 * - Crash: SIGCHLD via signalfd, reaped with waitpid(WNOHANG).
 * - Hang, restart backoff and the exit-code table: supervise.c.
 * - systemd WATCHDOG=1 from this main loop, never a side thread.
 * - Shutdown: SIGTERM to every child but store (reverse start order); store last,
 *   once the rest have exited or the grace ran out, so it records everything the
 *   hub sent. SIGKILL for anything still alive after its grace.
 * - Soak report every $DLBY_REPORT_S (report.h): one system line, then one line
 *   per child and for super itself (liveness, restarts, RSS, CPU, open fds). A
 *   final one when shutdown begins, and report_end before the exit line.
 *
 * The start line carries the wall-clock time and the build's git version, so a
 * soak log shows when it started and which build ran.
 *
 * $DLBY_BIN_DIR (default /opt/dlby/bin) holds the child binaries; $DLBY_CHILDREN
 * (comma list) limits which are run, keeping the DESIGN order. A missing binary
 * exits 78 and is not restarted.
 *
 * Not yet: SCHED_FIFO 90 + mlockall here and FIFO 60/70/80 for the alert chain
 * (mechanism C); forcing GPIO 17/22 low when alert dies (alert step).
 *
 * Exit codes: 0 after SIGTERM/SIGINT, 64 bad usage, 78 bad config, 70 otherwise.
 */
#include "ipc.h"
#include "log.h"
#include "msg.h"
#include "notify.h"
#include "procstat.h"
#include "report.h"
#include "supervise.h"
#include "version.h"

#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <limits.h>
#include <poll.h>
#include <signal.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/resource.h>
#include <sys/signalfd.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <sysexits.h>
#include <time.h>
#include <unistd.h>

#define BIN_DIR_DEFAULT "/opt/dlby/bin"
#define BIN_DIR_ENV     "DLBY_BIN_DIR"
#define CHILDREN_ENV    "DLBY_CHILDREN"
#define CHILD_HB_FD     3
#define WATCHDOG_NS     1000000000LL
#define STOP_GRACE_NS   2000000000LL
#define MAX_IDLE_MS     1000
#define MAX_CHILDREN    7

struct child_def {
	const char *name;
	int         nice;
	bool        last_to_stop;
};

/* DESIGN.md §2 E start order. TODO(mechanism C): FIFO 60 cap-*, 70 fuse-hub, 80 alert. */
static const struct child_def all_children[MAX_CHILDREN] = {
	{ "store",      5, true },	/* OTHER, nice +5 (DESIGN.md §1) */
	{ "fuse-hub",   0, false },
	{ "alert",      0, false },
	{ "status-lcd", 0, false },
	{ "cap-radar",  0, false },
	{ "cap-radar-left", 0, false },	/* same binary, left radar */
	{ "cap-sonar",  0, false },
};

struct child {
	const struct child_def *def;
	struct child_sm sm;
	pid_t pid;		/* -1 when not running */
	int   hb_fd;		/* super's end; -1 when closed */
	uint32_t starts;	/* successful starts; restarts = starts - 1 */
	char  path[PATH_MAX];
	struct log_limit lim_bad;
};

static struct child kids[MAX_CHILDREN];
static size_t nkids;

/* Shutdown progress: TERM the others, then store, then KILL what remains. */
struct shutdown {
	bool    active;
	bool    last_termed;	/* store has been sent SIGTERM */
	bool    killed_all;
	int64_t others_until;	/* grace for everything but store */
	int64_t last_until;	/* grace for store, from its SIGTERM */
};

static void close_keep_errno(int fd)
{
	int err = errno;

	(void)close(fd);	/* the fd is released even if close reports an error */
	errno = err;
}

static size_t token_len(const char *p)
{
	const char *comma = strchr(p, ',');

	return comma ? (size_t)(comma - p) : strlen(p);
}

static bool list_has(const char *list, const char *name)
{
	size_t len = strlen(name), tl;
	const char *p = list;

	for (;;) {
		tl = token_len(p);
		if (tl == len && strncmp(p, name, len) == 0)
			return true;
		if (p[tl] == '\0')
			return false;
		p += tl + 1;
	}
}

/* Every name in $DLBY_CHILDREN must be one of ours. */
static int check_list(const char *list)
{
	const char *p = list;
	size_t tl, i;

	for (;;) {
		tl = token_len(p);
		for (i = 0; i < MAX_CHILDREN; i++)
			if (strlen(all_children[i].name) == tl && strncmp(p, all_children[i].name, tl) == 0)
				break;
		if (i == MAX_CHILDREN) {
			log_msg(LOG_E, "bad_config %s=%s unknown=%.*s", CHILDREN_ENV, list, (int)tl, p);
			return -1;
		}
		if (p[tl] == '\0')
			return 0;
		p += tl + 1;
	}
}

static int select_children(const char *bin_dir)
{
	const char *list = getenv(CHILDREN_ENV);
	struct child *c;
	size_t i;
	int n;

	if (list && *list && check_list(list) < 0)
		return -1;
	for (i = 0; i < MAX_CHILDREN; i++) {
		if (list && *list && !list_has(list, all_children[i].name))
			continue;
		c = &kids[nkids++];
		memset(c, 0, sizeof(*c));
		c->def = &all_children[i];
		c->pid = -1;
		c->hb_fd = -1;
		sup_init(&c->sm);
		n = snprintf(c->path, sizeof(c->path), "%s/%s", bin_dir, c->def->name);
		if (n < 0 || (size_t)n >= sizeof(c->path)) {
			log_msg(LOG_E, "bad_config %s=%s err=too_long", BIN_DIR_ENV, bin_dir);
			return -1;
		}
	}
	return 0;
}

/* In the forked child: make the heartbeat end fd 3, then exec. Never returns. */
__attribute__((noreturn))
static void exec_child(const struct child *c, int fd)
{
	char pid_s[24];
	sigset_t none;
	int err;

	if (fd == CHILD_HB_FD) {
		if (fcntl(fd, F_SETFD, 0) < 0)
			goto fail;
	} else if (dup2(fd, CHILD_HB_FD) < 0) {	/* dup2 clears CLOEXEC on fd 3 */
		goto fail;
	}
	if (sigemptyset(&none) < 0 || sigprocmask(SIG_SETMASK, &none, NULL) < 0)
		goto fail;
	if (c->def->nice && setpriority(PRIO_PROCESS, 0, c->def->nice) < 0)
		goto fail;
	snprintf(pid_s, sizeof(pid_s), "%ld", (long)getppid());
	if (setenv("DLBY_HB_FD", "3", 1) < 0 || setenv("DLBY_SUPER_PID", pid_s, 1) < 0)
		goto fail;
	execl(c->path, c->def->name, (char *)NULL);
	err = errno;
	log_msg(LOG_E, "exec_failed name=%s path=%s err=%s", c->def->name, c->path, strerror(err));
	_exit(err == ENOENT || err == EACCES || err == ENOTDIR ? EX_CONFIG : EX_SOFTWARE);
fail:
	log_msg(LOG_E, "child_setup_failed name=%s err=%s", c->def->name, strerror(errno));
	_exit(EX_SOFTWARE);
}

static void decide(struct child *c, int status, int64_t now)
{
	switch (sup_on_exit(&c->sm, status, now)) {
	case SUP_EXIT_RESTART:
		log_msg(LOG_I, "child_restart name=%s in_ms=%d", c->def->name, c->sm.delay_ms);
		break;
	case SUP_EXIT_DISABLED:
		log_msg(LOG_E, "child_disabled name=%s code=%d", c->def->name, EX_CONFIG);
		break;
	case SUP_EXIT_STOPPED:
		log_msg(LOG_I, "child_stopped name=%s restart=no", c->def->name);
		break;
	}
}

static void start_child(struct child *c, int64_t now)
{
	int sv[2];
	pid_t pid;

	if (socketpair(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0, sv) < 0)
		goto fail;
	pid = fork();
	if (pid < 0) {
		close_keep_errno(sv[0]);
		close_keep_errno(sv[1]);
		goto fail;
	}
	if (pid == 0)
		exec_child(c, sv[1]);
	if (close(sv[1]) < 0)
		log_msg(LOG_W, "close_failed err=%s", strerror(errno));
	c->pid = pid;
	c->hb_fd = sv[0];
	c->starts++;
	sup_on_start(&c->sm, now);
	log_msg(LOG_I, "child_start name=%s pid=%ld", c->def->name, (long)pid);
	return;

fail:
	log_msg(LOG_E, "start_failed name=%s err=%s", c->def->name, strerror(errno));
	sup_on_start(&c->sm, now);
	decide(c, W_EXITCODE(EX_SOFTWARE, 0), now);
}

static void signal_child(const struct child *c, int sig)
{
	if (c->pid > 0 && kill(c->pid, sig) < 0 && errno != ESRCH)
		log_msg(LOG_W, "kill_failed name=%s err=%s", c->def->name, strerror(errno));
}

static void kill_child(const struct child *c)
{
	if (c->pid > 0) {
		log_msg(LOG_W, "child_kill name=%s signal=KILL", c->def->name);
		signal_child(c, SIGKILL);
	}
}

static void close_hb(struct child *c)
{
	if (c->hb_fd >= 0) {
		close_keep_errno(c->hb_fd);
		c->hb_fd = -1;
	}
}

static void read_hb(struct child *c, int64_t now)
{
	uint8_t buf[MSG_MAX];
	uint64_t rejected = 0;
	ssize_t n;

	n = ipc_recv(c->hb_fd, buf, sizeof(buf), &rejected);
	if (n < 0) {
		if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR)
			return;
		if (errno == EMSGSIZE) {
			log_limited(&c->lim_bad, now, LOG_W, "bad_hb name=%s reason=trunc", c->def->name);
			return;
		}
		log_msg(LOG_W, "hb_recv_failed name=%s err=%s", c->def->name, strerror(errno));
		close_hb(c);
		return;
	}
	if (n == 0) {
		close_hb(c);	/* the child is going; its SIGCHLD follows */
		return;
	}
	if (ipc_check_msg(buf, (size_t)n, &rejected) != MSG_HB) {
		log_limited(&c->lim_bad, now, LOG_W, "bad_hb name=%s len=%zd", c->def->name, n);
		return;
	}
	if (sup_on_hb(&c->sm, now))
		log_msg(LOG_I, "first_heartbeat name=%s ms=%" PRId64, c->def->name,
			(now - c->sm.start_ns) / 1000000);
}

static void on_child_exit(struct child *c, int status, int64_t now, bool stopping)
{
	const char *sig;

	close_hb(c);
	c->pid = -1;
	if (WIFSIGNALED(status)) {
		sig = sigabbrev_np(WTERMSIG(status));
		if (sig)
			log_msg(LOG_E, "child_exit name=%s signal=%s", c->def->name, sig);
		else
			log_msg(LOG_E, "child_exit name=%s signal=%d", c->def->name, WTERMSIG(status));
	} else {
		log_msg(WEXITSTATUS(status) == 0 ? LOG_I : LOG_E, "child_exit name=%s code=%d",
			c->def->name, WEXITSTATUS(status));
	}
	if (stopping)
		return;
	/* TODO(alert step): if this is alert, drive GPIO 17/22 low and hold them
	 * until it is restarted (DESIGN.md §2 E "Stuck outputs"). */
	decide(c, status, now);
}

/* Reap every exited child. Returns -1 only on an unexpected waitpid() error. */
static int reap(int64_t now, bool stopping)
{
	pid_t pid;
	size_t i;
	int st;

	for (;;) {
		pid = waitpid(-1, &st, WNOHANG);
		if (pid == 0)
			return 0;
		if (pid < 0) {
			if (errno == ECHILD)
				return 0;
			if (errno == EINTR)
				continue;
			log_msg(LOG_E, "waitpid_failed err=%s", strerror(errno));
			return -1;
		}
		for (i = 0; i < nkids; i++) {
			if (kids[i].pid == pid) {
				on_child_exit(&kids[i], st, now, stopping);
				break;
			}
		}
	}
}

/* Children still running; with others_only, not counting store. */
static size_t alive(bool others_only)
{
	size_t i, n = 0;

	for (i = 0; i < nkids; i++)
		if (kids[i].pid > 0 && !(others_only && kids[i].def->last_to_stop))
			n++;
	return n;
}

static void begin_shutdown(struct shutdown *sd, int64_t now)
{
	size_t i;

	sd->active = true;
	sd->others_until = now + STOP_GRACE_NS;
	for (i = nkids; i-- > 0;)		/* reverse start order */
		if (!kids[i].def->last_to_stop)
			signal_child(&kids[i], SIGTERM);
}

/* One shutdown step. Returns true when every child has exited. */
static bool shutdown_step(struct shutdown *sd, int64_t now)
{
	size_t i;

	if (!sd->last_termed && (alive(true) == 0 || now >= sd->others_until)) {
		for (i = 0; i < nkids; i++)
			if (!kids[i].def->last_to_stop)
				kill_child(&kids[i]);		/* only those past their grace */
		for (i = 0; i < nkids; i++)
			if (kids[i].def->last_to_stop)
				signal_child(&kids[i], SIGTERM);
		sd->last_termed = true;
		sd->last_until = now + STOP_GRACE_NS;
	}
	if (alive(false) == 0)
		return true;
	if (sd->last_termed && !sd->killed_all && now >= sd->last_until) {
		for (i = 0; i < nkids; i++)
			kill_child(&kids[i]);
		sd->killed_all = true;
	}
	return false;
}

static int64_t shutdown_due(const struct shutdown *sd)
{
	if (!sd->last_termed)
		return sd->others_until;
	if (!sd->killed_all)
		return sd->last_until;
	return INT64_MAX;
}

static void notify(const char *msg, struct log_limit *lim, int64_t now)
{
	if (notify_send(msg) < 0)
		log_limited(lim, now, LOG_W, "notify_failed msg=%s err=%s", msg, strerror(errno));
}

/* Soak report state (report.h). */
struct sysrep {
	struct report r;
	int64_t  start_ns;
	long     clk_tck;
	bool     have_prev;
	uint64_t prev_total, prev_busy;
};

static const char *state_name(enum child_state s)
{
	switch (s) {
	case CH_IDLE:         return "idle";
	case CH_RUNNING:      return "running";
	case CH_TERMINATING:  return "terminating";
	case CH_KILLING:      return "killing";
	case CH_WAIT_RESTART: return "wait_restart";
	case CH_DISABLED:     return "disabled";
	case CH_STOPPED:      return "stopped";
	}
	return "unknown";
}

/* Remember the first failure of a report as " err=<what>:<why>" for its line. */
static void note_err(char *err, size_t cap, const char *what)
{
	if (!err[0])
		snprintf(err, cap, " err=%s:%s", what, strerror(errno));
}

/* One process's RSS, CPU seconds and open fds; -1 for any that can't be read. */
static void read_proc(const char *dir, pid_t pid, long clk_tck, long *rss_kb, double *cpu_s,
		      int *fds, char *err, size_t cap)
{
	char path[64], buf[PROCSTAT_BUF];
	uint64_t ticks;

	*rss_kb = -1;
	*cpu_s = -1;
	snprintf(path, sizeof(path), "%s/status", dir);
	if (procstat_read(path, buf, sizeof(buf)) < 0 || procstat_rss_kb(buf, rss_kb) < 0) {
		*rss_kb = -1;
		note_err(err, cap, "status");
	}
	snprintf(path, sizeof(path), "%s/stat", dir);
	if (procstat_read(path, buf, sizeof(buf)) < 0 || procstat_cpu_ticks(buf, &ticks) < 0)
		note_err(err, cap, "stat");
	else
		*cpu_s = (double)ticks / (double)clk_tck;
	*fds = procstat_count_fds(pid);
	if (*fds < 0)
		note_err(err, cap, "fd");
}

static void report_system(struct sysrep *sr, int64_t now, bool final)
{
	char buf[PROCSTAT_BUF], err[96] = "";
	uint64_t total, idle, busy;
	double cpu_pct = -1, load1 = -1;
	long avail = -1, temp = -1;
	char *end;

	if (procstat_read("/proc/stat", buf, sizeof(buf)) < 0 ||
	    procstat_sys_cpu(buf, &total, &idle) < 0) {
		note_err(err, sizeof(err), "/proc/stat");
	} else {
		/* iowait can step backwards, so busy is clamped, never negative. */
		busy = total > idle ? total - idle : 0;
		if (sr->have_prev && total > sr->prev_total)
			cpu_pct = busy > sr->prev_busy ?
				  100.0 * (double)(busy - sr->prev_busy) /
				  (double)(total - sr->prev_total) : 0.0;
		sr->prev_total = total;
		sr->prev_busy = busy;
		sr->have_prev = true;
	}
	if (procstat_read("/proc/loadavg", buf, sizeof(buf)) < 0 || procstat_load1(buf, &load1) < 0)
		note_err(err, sizeof(err), "/proc/loadavg");
	if (procstat_read("/proc/meminfo", buf, sizeof(buf)) < 0 ||
	    procstat_mem_avail_kb(buf, &avail) < 0)
		note_err(err, sizeof(err), "/proc/meminfo");
	/* No thermal zone (a container) is normal: temp_mc=-1, not an error. */
	if (procstat_read("/sys/class/thermal/thermal_zone0/temp", buf, sizeof(buf)) >= 0) {
		errno = 0;
		temp = strtol(buf, &end, 10);
		if (errno || end == buf) {
			temp = -1;
			errno = EINVAL;
			note_err(err, sizeof(err), "thermal");
		}
	} else if (errno != ENOENT) {
		note_err(err, sizeof(err), "thermal");
	}
	log_msg(LOG_I, "report n=%" PRIu32 " uptime_s=%" PRId64 " cpu_pct=%.1f load1=%.2f"
		" mem_avail_kb=%ld temp_mc=%ld%s%s", sr->r.n, (now - sr->start_ns) / 1000000000,
		cpu_pct, load1, avail, temp, final ? " final=1" : "", err);
}

static void report_all(struct sysrep *sr, int64_t now, bool final)
{
	const char *fin = final ? " final=1" : "";
	char err[96];
	long rss;
	double cpu;
	int fds;
	size_t i;
	struct child *c;

	report_system(sr, now, final);

	err[0] = '\0';
	read_proc("/proc/self", getpid(), sr->clk_tck, &rss, &cpu, &fds, err, sizeof(err));
	if (fds > 0)
		fds--;		/* the directory being counted */
	log_msg(LOG_I, "report_child n=%" PRIu32 " name=dlby-super pid=%ld up=1 state=running"
		" restarts=0 rss_kb=%ld cpu_s=%.2f fds=%d%s%s", sr->r.n, (long)getpid(), rss, cpu,
		fds, fin, err);

	for (i = 0; i < nkids; i++) {
		char dir[32];

		c = &kids[i];
		err[0] = '\0';
		rss = -1;
		cpu = -1;
		fds = -1;
		if (c->pid > 0) {
			snprintf(dir, sizeof(dir), "/proc/%ld", (long)c->pid);
			read_proc(dir, c->pid, sr->clk_tck, &rss, &cpu, &fds, err, sizeof(err));
		}
		log_msg(LOG_I, "report_child n=%" PRIu32 " name=%s pid=%ld up=%d state=%s restarts=%"
			PRIu32 " rss_kb=%ld cpu_s=%.2f fds=%d%s%s", sr->r.n, c->def->name, (long)c->pid,
			c->pid > 0 && c->sm.state == CH_RUNNING && c->sm.got_hb,
			state_name(c->sm.state), c->starts ? c->starts - 1 : 0, rss, cpu, fds, fin, err);
	}
}

/* Wall-clock time as ISO 8601 UTC, or "unknown" (logged) if it can't be read. */
static void wall_now(char *buf, size_t cap)
{
	struct timespec ts;
	struct tm tm;

	if (clock_gettime(CLOCK_REALTIME, &ts) < 0 || !gmtime_r(&ts.tv_sec, &tm) ||
	    strftime(buf, cap, "%Y-%m-%dT%H:%M:%SZ", &tm) == 0) {
		log_msg(LOG_W, "wall_clock_failed err=%s", strerror(errno));
		snprintf(buf, cap, "unknown");
	}
}

static int signal_fd(void)
{
	sigset_t mask;

	if (sigemptyset(&mask) < 0 || sigaddset(&mask, SIGCHLD) < 0 ||
	    sigaddset(&mask, SIGTERM) < 0 || sigaddset(&mask, SIGINT) < 0)
		return -1;
	if (sigprocmask(SIG_BLOCK, &mask, NULL) < 0)
		return -1;
	return signalfd(-1, &mask, SFD_CLOEXEC | SFD_NONBLOCK);
}

static int timeout_ms(int64_t due, int64_t now)
{
	int64_t ms;

	if (due == INT64_MAX)
		return MAX_IDLE_MS;
	if (due <= now)
		return 0;
	ms = (due - now + 999999) / 1000000;
	return ms > MAX_IDLE_MS ? MAX_IDLE_MS : (int)ms;
}

int main(int argc, char **argv)
{
	struct pollfd pfd[1 + MAX_CHILDREN];
	size_t slot[1 + MAX_CHILDREN];
	struct signalfd_siginfo si;
	struct shutdown sd = { 0 };
	struct log_limit lim_notify = { 0 };
	struct sysrep sr = { 0 };
	const char *bin_dir;
	char names[128] = "", wall[32];
	int64_t now, next_wd, due, d;
	bool ready_sent = false;
	int sfd = -1, rc = EX_SOFTWARE, n;
	uint32_t restarts;
	size_t i, nfds;
	ssize_t r;

	(void)argv;
	log_init("dlby-super");
	if (argc > 1) {
		log_msg(LOG_E, "usage args=none");
		return EX_USAGE;
	}
	bin_dir = getenv(BIN_DIR_ENV);
	if (!bin_dir || !*bin_dir)
		bin_dir = BIN_DIR_DEFAULT;
	if (select_children(bin_dir) < 0)
		return EX_CONFIG;
	for (i = 0; i < nkids; i++) {
		strncat(names, i ? "," : "", sizeof(names) - strlen(names) - 1);
		strncat(names, kids[i].def->name, sizeof(names) - strlen(names) - 1);
	}

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
	if (report_init(&sr.r, now) < 0) {
		log_msg(LOG_E, "bad_config %s=%s want=1..%d", REPORT_ENV, getenv(REPORT_ENV),
			REPORT_MAX_S);
		rc = EX_CONFIG;
		goto out;
	}
	sr.start_ns = now;
	sr.clk_tck = sysconf(_SC_CLK_TCK);
	if (sr.clk_tck <= 0) {
		log_msg(LOG_W, "clk_tck_failed err=%s assume=100", strerror(errno));
		sr.clk_tck = 100;
	}
	wall_now(wall, sizeof(wall));
	log_msg(LOG_I, "start wall=%s version=%s report_s=%" PRId64 " bin_dir=%s children=%s",
		wall, DLBY_VERSION, sr.r.period_ns / 1000000000, bin_dir, names);
	report_system(&sr, now, false);	/* CPU baseline; logged as report n=0 */
	next_wd = now;

	for (;;) {
		if (!sd.active) {
			for (i = 0; i < nkids; i++) {
				switch (sup_tick(&kids[i].sm, now)) {
				case SUP_START:
					start_child(&kids[i], now);
					break;
				case SUP_TERM:
					log_msg(LOG_E, "heartbeat_lost name=%s", kids[i].def->name);
					signal_child(&kids[i], SIGTERM);
					break;
				case SUP_KILL:
					kill_child(&kids[i]);
					break;
				case SUP_NONE:
					break;
				}
			}
			if (!ready_sent) {
				notify("READY=1", &lim_notify, now);
				ready_sent = true;
			}
			if (report_due(&sr.r, now))
				report_all(&sr, now, false);
		} else if (shutdown_step(&sd, now)) {
			rc = EX_OK;
			goto out;
		}
		/* From this loop only, so a hung super stops the pings (§3 last row). */
		if (now >= next_wd) {
			notify("WATCHDOG=1", &lim_notify, now);
			next_wd = now + WATCHDOG_NS;
		}

		pfd[0] = (struct pollfd){ .fd = sfd, .events = POLLIN };
		nfds = 1;
		due = next_wd;
		for (i = 0; i < nkids; i++) {
			if (kids[i].hb_fd >= 0) {
				slot[nfds] = i;
				pfd[nfds++] = (struct pollfd){ .fd = kids[i].hb_fd, .events = POLLIN };
			}
			d = sup_deadline(&kids[i].sm);
			if (!sd.active && d < due)
				due = d;
		}
		if (sd.active && shutdown_due(&sd) < due)
			due = shutdown_due(&sd);
		if (!sd.active && sr.r.next_ns < due)
			due = sr.r.next_ns;

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
				while ((r = read(sfd, &si, sizeof(si))) == (ssize_t)sizeof(si)) {
					if (si.ssi_signo == SIGCHLD || sd.active)
						continue;
					log_msg(LOG_I, "stop signal=%s", sigabbrev_np((int)si.ssi_signo));
					notify("STOPPING=1", &lim_notify, now);
					/* While every child can still be read. */
					report_final(&sr.r);
					report_all(&sr, now, true);
					begin_shutdown(&sd, now);
				}
				if (r >= 0 || (errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR)) {
					log_msg(LOG_E, "signalfd_read_failed err=%s",
						r < 0 ? strerror(errno) : "short");
					goto out;
				}
			}
			for (i = 1; i < nfds; i++)
				if (pfd[i].revents && kids[slot[i]].hb_fd >= 0)
					read_hb(&kids[slot[i]], now);
		}
		/* Every pass, so a coalesced SIGCHLD can't leave a zombie behind. */
		if (reap(now, sd.active) < 0)
			goto out;
	}

out:
	for (i = 0; i < nkids; i++)
		close_hb(&kids[i]);
	if (sfd >= 0 && close(sfd) < 0)
		log_msg(LOG_W, "close_failed err=%s", strerror(errno));
	restarts = 0;
	for (i = 0; i < nkids; i++)
		restarts += kids[i].starts ? kids[i].starts - 1 : 0;
	now = dlby_now_ns();
	if (sr.start_ns)	/* only once the start line was logged */
		log_msg(LOG_I, "report_end reports=%" PRIu32 " uptime_s=%" PRId64 " restarts=%"
			PRIu32, sr.r.n, now >= 0 ? (now - sr.start_ns) / 1000000000 : -1, restarts);
	log_msg(rc == EX_OK ? LOG_I : LOG_E, "exit code=%d", rc);
	return rc;
}
