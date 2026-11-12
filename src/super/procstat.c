#include "procstat.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static int bad(void)
{
	errno = EINVAL;
	return -1;
}

/* The value after "<key>" at the start of a line, as a long. */
static int line_long(const char *text, const char *key, long *out)
{
	size_t kl = strlen(key);
	const char *p = text;
	char *end;
	long v;

	while (p && *p) {
		if (strncmp(p, key, kl) == 0) {
			errno = 0;
			v = strtol(p + kl, &end, 10);
			if (errno || end == p + kl || v < 0)
				return bad();
			*out = v;
			return 0;
		}
		p = strchr(p, '\n');
		if (p)
			p++;
	}
	return bad();
}

int procstat_rss_kb(const char *status, long *kb)
{
	return line_long(status, "VmRSS:", kb);
}

int procstat_mem_avail_kb(const char *meminfo, long *kb)
{
	return line_long(meminfo, "MemAvailable:", kb);
}

int procstat_cpu_ticks(const char *stat, uint64_t *ticks)
{
	const char *p = strrchr(stat, ')');
	unsigned long long ut = 0, st = 0, v;
	char *end;
	int field;

	if (!p)
		return bad();
	p++;
	/* After ')': field 3 (state) is a letter, then numbers; utime is 14, stime 15. */
	for (field = 3; field <= 15; field++) {
		while (*p == ' ')
			p++;
		if (!*p || *p == '\n')
			return bad();
		if (field == 3) {
			p++;
			continue;
		}
		errno = 0;
		v = strtoull(p, &end, 10);
		if (errno || end == p)
			return bad();
		if (field == 14)
			ut = v;
		else if (field == 15)
			st = v;
		p = end;
	}
	*ticks = ut + st;
	return 0;
}

int procstat_sys_cpu(const char *stat, uint64_t *total, uint64_t *idle)
{
	unsigned long long f[8], sum = 0;
	const char *p;
	char *end;
	int i;

	if (strncmp(stat, "cpu ", 4) != 0)
		return bad();
	p = stat + 4;
	for (i = 0; i < 8; i++) {
		while (*p == ' ')
			p++;
		/* Never run onto the next line (a cut-off or short cpu line). */
		if (*p < '0' || *p > '9')
			return bad();
		errno = 0;
		f[i] = strtoull(p, &end, 10);
		if (errno || end == p)
			return bad();
		sum += f[i];
		p = end;
	}
	*total = sum;
	*idle = f[3] + f[4];
	return 0;
}

int procstat_load1(const char *loadavg, double *load1)
{
	char *end;
	double v;

	errno = 0;
	v = strtod(loadavg, &end);
	if (errno || end == loadavg || v < 0)
		return bad();
	*load1 = v;
	return 0;
}

ssize_t procstat_read(const char *path, char *buf, size_t cap)
{
	size_t n = 0;
	ssize_t r;
	int fd, err;

	if (cap < 2) {
		errno = EINVAL;
		return -1;
	}
	fd = open(path, O_RDONLY | O_CLOEXEC);
	if (fd < 0)
		return -1;
	while (n < cap - 1) {
		r = read(fd, buf + n, cap - 1 - n);
		if (r < 0) {
			if (errno == EINTR)
				continue;
			err = errno;
			(void)close(fd);	/* released even if close reports an error */
			errno = err;
			return -1;
		}
		if (r == 0)
			break;
		n += (size_t)r;
	}
	buf[n] = '\0';
	if (close(fd) < 0)
		return -1;
	return (ssize_t)n;
}

int procstat_count_fds(pid_t pid)
{
	char path[64];
	struct dirent *e;
	DIR *d;
	int n = 0, err;

	snprintf(path, sizeof(path), "/proc/%ld/fd", (long)pid);
	d = opendir(path);
	if (!d)
		return -1;
	for (;;) {
		errno = 0;
		e = readdir(d);
		if (!e)
			break;
		if (e->d_name[0] != '.')
			n++;
	}
	err = errno;
	if (closedir(d) < 0 && !err)
		err = errno;
	if (err) {
		errno = err;
		return -1;
	}
	return n;
}
