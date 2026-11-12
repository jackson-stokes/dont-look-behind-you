/* /proc readers for dlby-super's soak report (report.h). The parsers are pure,
 * text in, numbers out, so they are unit-tested on captured /proc text; the
 * readers below them do the I/O.
 */
#ifndef DLBY_PROCSTAT_H
#define DLBY_PROCSTAT_H

#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>

#define PROCSTAT_BUF 4096

/* Each returns 0, or -1 with EINVAL if the field is missing or malformed. */

/* /proc/<pid>/status: VmRSS in kB (absent for zombies and kernel threads). */
int procstat_rss_kb(const char *status, long *kb);

/* /proc/<pid>/stat: utime + stime in clock ticks. The comm field may hold
 * spaces and parentheses, so fields are counted from the last ')'. */
int procstat_cpu_ticks(const char *stat, uint64_t *ticks);

/* /proc/stat first line: total = user..steal; idle = idle + iowait. */
int procstat_sys_cpu(const char *stat, uint64_t *total, uint64_t *idle);

/* /proc/meminfo: MemAvailable in kB. */
int procstat_mem_avail_kb(const char *meminfo, long *kb);

/* /proc/loadavg: the 1-minute load. */
int procstat_load1(const char *loadavg, double *load1);

/* Reads the first cap-1 bytes of a file into buf, NUL-terminated; a longer file
 * is cut off, not an error. Every field parsed here sits near the start of its
 * file: /proc/stat can run past PROCSTAT_BUF (the intr line lists every IRQ),
 * but the cpu line is its first line. A field cut off would fail its parser
 * with EINVAL, never read as a wrong value. Returns the length, or -1 with errno. */
ssize_t procstat_read(const char *path, char *buf, size_t cap);

/* Open fds of pid: entries in /proc/<pid>/fd. Returns the count or -1 with errno. */
int procstat_count_fds(pid_t pid);

#endif
