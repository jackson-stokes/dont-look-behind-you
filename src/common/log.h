/* Log lines in the DESIGN.md §3 format, on stderr (journald stores them):
 *
 *   <proc>[<pid>] <I|W|E> <event> key=value ...
 *
 * The format string starts with the event name: log_msg(LOG_W, "degraded lost=%s",
 * "radar"). Each line is one write() of at most LOG_LINE_MAX bytes; longer lines
 * end in "...". Logging never changes errno.
 */
#ifndef DLBY_LOG_H
#define DLBY_LOG_H

#include <stdbool.h>
#include <stdint.h>

#define LOG_LINE_MAX 512
#define LOG_LIMIT_NS (10LL * 1000000000LL)

enum log_level {
	LOG_I = 'I',
	LOG_W = 'W',
	LOG_E = 'E',
};

/* Rate limit for one repeating event: keep one per call site, zero-initialised. */
struct log_limit {
	int64_t  last_ns;
	uint32_t count;
	bool     primed;
};

void log_init(const char *proc);

void log_msg(enum log_level lv, const char *fmt, ...)
	__attribute__((format(printf, 2, 3)));

/* At most one line per 10 s, ending in count=N: the occurrences since the last
 * printed line, including this one. now_ns is CLOCK_MONOTONIC. */
void log_limited(struct log_limit *lim, int64_t now_ns, enum log_level lv, const char *fmt, ...)
	__attribute__((format(printf, 4, 5)));

#endif
