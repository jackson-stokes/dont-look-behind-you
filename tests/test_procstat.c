#include "check.h"
#include "procstat.h"
#include "report.h"

#include <unistd.h>

#define SEC_NS 1000000000LL

/* Real /proc text (Linux 6.x, aarch64), trimmed. */
static const char STATUS[] =
	"Name:\tcat\nUmask:\t0022\nState:\tR (running)\nTgid:\t12\nPid:\t12\n"
	"VmPeak:\t    2584 kB\nVmSize:\t    2584 kB\nVmLck:\t       0 kB\nVmHWM:\t    1440 kB\n"
	"VmRSS:\t    1440 kB\nRssAnon:\t       0 kB\nThreads:\t1\n";
static const char STAT_PID[] =
	"12 (cat) R 1 1 1 0 -1 4194304 47 0 1 0 7 3 0 0 20 0 1 0 105 2646016 260 "
	"18446744073709551615 187651058106368 187651058147400 0 0 0 0 0 0 0 0 0 17 5 0 0\n";
static const char STAT_SYS[] =
	"cpu  59 0 77 648 26 0 17 0 0 0\n"
	"cpu0 5 0 10 75 5 0 10 0 0 0\n"
	"intr 19157 0 661 9251 0 0 0 33 0 0 0 0 6780 0 0 103 372\n";
static const char MEMINFO[] =
	"MemTotal:        8022576 kB\nMemFree:         7346060 kB\nMemAvailable:    7521272 kB\n";

static void test_rss(void)
{
	long kb = 0;

	CHECK(procstat_rss_kb(STATUS, &kb) == 0 && kb == 1440);
	/* VmRSS at the very start of the text, and missing (a zombie). */
	CHECK(procstat_rss_kb("VmRSS:\t 7 kB\n", &kb) == 0 && kb == 7);
	errno = 0;
	CHECK(procstat_rss_kb("Name:\tx\nState:\tZ (zombie)\n", &kb) == -1 && errno == EINVAL);
	CHECK(procstat_rss_kb("VmRSS:\t kB\n", &kb) == -1);
	/* A key that only ends another one isn't it. */
	CHECK(procstat_rss_kb("XVmRSS:\t 9 kB\n", &kb) == -1);
}

static void test_cpu_ticks(void)
{
	uint64_t t = 0;

	CHECK(procstat_cpu_ticks(STAT_PID, &t) == 0 && t == 10);	/* utime 7 + stime 3 */
	/* comm with spaces and a ')' inside. */
	CHECK(procstat_cpu_ticks("9 (a) b) c) S 1 1 1 0 -1 0 0 0 0 0 40 2 0 0\n", &t) == 0 &&
	      t == 42);
	CHECK(procstat_cpu_ticks("9 (x) S 1 1 1 0 -1 0 0 0 0 0 40\n", &t) == -1);	/* short */
	CHECK(procstat_cpu_ticks("no paren", &t) == -1);
}

static void test_sys_cpu(void)
{
	uint64_t total = 0, idle = 0;

	CHECK(procstat_sys_cpu(STAT_SYS, &total, &idle) == 0);
	CHECK(total == 59 + 77 + 648 + 26 + 17);	/* user..steal; guest is inside user */
	CHECK(idle == 648 + 26);
	CHECK(procstat_sys_cpu("cpu0 1 2 3 4 5 6 7 8\n", &total, &idle) == -1);
	/* A cut-off cpu line must fail, not read the next line's numbers. */
	CHECK(procstat_sys_cpu("cpu  1 2 3\ncpu0 4 5 6 7 8 9 10 11\n", &total, &idle) == -1);
}

static void test_meminfo_load(void)
{
	long kb = 0;
	double l = -1;

	CHECK(procstat_mem_avail_kb(MEMINFO, &kb) == 0 && kb == 7521272);
	CHECK(procstat_mem_avail_kb("MemTotal: 1 kB\n", &kb) == -1);
	CHECK(procstat_load1("0.42 0.10 0.00 3/233 14\n", &l) == 0 && l > 0.41 && l < 0.43);
	CHECK(procstat_load1("", &l) == -1);
}

/* /proc/stat on a Pi runs past PROCSTAT_BUF (the intr line lists every IRQ):
 * reading the head must still give the cpu line. */
static void test_read_long_file_head(void)
{
	char path[] = "/tmp/dlby-procstat-XXXXXX", buf[PROCSTAT_BUF];
	uint64_t total, idle;
	ssize_t n;
	int fd, i;

	fd = mkstemp(path);
	CHECK(fd >= 0);
	CHECK(write(fd, "cpu  10 0 20 300 5 0 1 0 0 0\nintr 1", 35) == 35);
	for (i = 0; i < 3000; i++)
		CHECK(write(fd, " 12345", 6) == 6);	/* ~18 KB */
	CHECK(write(fd, "\n", 1) == 1);
	CHECK(close(fd) == 0);
	n = procstat_read(path, buf, sizeof(buf));
	CHECK(n == (ssize_t)sizeof(buf) - 1 && buf[n] == '\0');
	CHECK(procstat_sys_cpu(buf, &total, &idle) == 0 && total == 336 && idle == 305);
	CHECK(unlink(path) == 0);
	errno = 0;
	CHECK(procstat_read("/nonexistent/x", buf, sizeof(buf)) == -1 && errno == ENOENT);
	CHECK(procstat_read(path, buf, 1) == -1 && errno == EINVAL);
}

/* The live /proc of this process and system parses. */
static void test_live(void)
{
	char buf[PROCSTAT_BUF];
	uint64_t t, total, idle;
	long kb;
	int fds;

	CHECK(procstat_read("/proc/self/status", buf, sizeof(buf)) > 0);
	CHECK(procstat_rss_kb(buf, &kb) == 0 && kb > 0);
	CHECK(procstat_read("/proc/self/stat", buf, sizeof(buf)) > 0);
	CHECK(procstat_cpu_ticks(buf, &t) == 0);
	CHECK(procstat_read("/proc/stat", buf, sizeof(buf)) > 0);
	CHECK(procstat_sys_cpu(buf, &total, &idle) == 0 && total >= idle);
	fds = procstat_count_fds(getpid());
	CHECK(fds >= 3);	/* stdin/out/err, plus the directory being read */
	errno = 0;
	CHECK(procstat_count_fds(999999999) == -1 && errno == ENOENT);
}

static void test_report_parse(void)
{
	int64_t ns = 0;

	CHECK(report_parse_s(NULL, &ns) == 0 && ns == REPORT_DEFAULT_S * SEC_NS);
	CHECK(report_parse_s("", &ns) == 0 && ns == REPORT_DEFAULT_S * SEC_NS);
	CHECK(report_parse_s("1", &ns) == 0 && ns == SEC_NS);
	CHECK(report_parse_s("3600", &ns) == 0 && ns == 3600 * SEC_NS);
	errno = 0;
	CHECK(report_parse_s("3601", &ns) == -1 && errno == EINVAL);	/* over hourly */
	CHECK(report_parse_s("0", &ns) == -1);
	CHECK(report_parse_s("-5", &ns) == -1);
	CHECK(report_parse_s("60s", &ns) == -1);
	CHECK(report_parse_s("abc", &ns) == -1);
	CHECK(report_parse_s("99999999999999999999", &ns) == -1);
}

static void test_report_schedule(void)
{
	struct report r;

	CHECK(unsetenv(REPORT_ENV) == 0);
	CHECK(report_init(&r, 5 * SEC_NS) == 0 && r.period_ns == REPORT_DEFAULT_S * SEC_NS);
	CHECK(setenv(REPORT_ENV, "10", 1) == 0);
	CHECK(report_init(&r, 0) == 0);
	CHECK(!report_due(&r, 10 * SEC_NS - 1));
	CHECK(report_due(&r, 10 * SEC_NS) && r.n == 1);
	CHECK(!report_due(&r, 10 * SEC_NS));
	/* Late by 3 s: the next is still at 20 s, so reports don't drift. */
	CHECK(!report_due(&r, 13 * SEC_NS));
	CHECK(report_due(&r, 20 * SEC_NS) && r.n == 2 && r.next_ns == 30 * SEC_NS);
	CHECK(report_due(&r, 33 * SEC_NS) && r.next_ns == 40 * SEC_NS);
	/* Stalled for more than a period: one report, then one period on. */
	CHECK(report_due(&r, 75 * SEC_NS) && r.next_ns == 85 * SEC_NS && r.n == 4);
	CHECK(!report_due(&r, 84 * SEC_NS));
	report_final(&r);
	CHECK(r.n == 5);
	CHECK(setenv(REPORT_ENV, "7200", 1) == 0);
	CHECK(report_init(&r, 0) == -1 && errno == EINVAL);
	CHECK(unsetenv(REPORT_ENV) == 0);
}

int main(void)
{
	RUN(test_rss);
	RUN(test_cpu_ticks);
	RUN(test_sys_cpu);
	RUN(test_meminfo_load);
	RUN(test_read_long_file_head);
	RUN(test_live);
	RUN(test_report_parse);
	RUN(test_report_schedule);
	return 0;
}
