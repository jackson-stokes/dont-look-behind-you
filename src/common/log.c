#include "log.h"

#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

static char proc_name[32] = "dlby";

struct linebuf {
	char   b[LOG_LINE_MAX];
	size_t n;
	bool   trunc;
};

void log_init(const char *proc)
{
	snprintf(proc_name, sizeof(proc_name), "%s", proc);
}

/* Append, always keeping one byte free for the trailing '\n'. */
static void lb_vadd(struct linebuf *lb, const char *fmt, va_list ap)
{
	size_t room = sizeof(lb->b) - 1 - lb->n;
	int r;

	if (lb->trunc)
		return;
	r = vsnprintf(lb->b + lb->n, room + 1, fmt, ap);
	if (r < 0 || (size_t)r > room) {
		lb->n += r < 0 ? 0 : room;
		lb->trunc = true;
		return;
	}
	lb->n += (size_t)r;
}

__attribute__((format(printf, 2, 3)))
static void lb_add(struct linebuf *lb, const char *fmt, ...)
{
	va_list ap;

	va_start(ap, fmt);
	lb_vadd(lb, fmt, ap);
	va_end(ap);
}

static void emit(enum log_level lv, uint32_t count, const char *fmt, va_list ap)
{
	struct linebuf lb = { .n = 0, .trunc = false };
	int err = errno;
	ssize_t w;

	lb_add(&lb, "%s[%ld] %c ", proc_name, (long)getpid(), (int)lv);
	lb_vadd(&lb, fmt, ap);
	if (count)
		lb_add(&lb, " count=%u", (unsigned)count);
	if (lb.trunc && lb.n >= 3)
		memcpy(lb.b + lb.n - 3, "...", 3);
	lb.b[lb.n++] = '\n';

	/* A failed or short write to stderr has nowhere to be reported; drop it. */
	do {
		w = write(STDERR_FILENO, lb.b, lb.n);
	} while (w < 0 && errno == EINTR);
	errno = err;
}

void log_msg(enum log_level lv, const char *fmt, ...)
{
	va_list ap;

	va_start(ap, fmt);
	emit(lv, 0, fmt, ap);
	va_end(ap);
}

void log_limited(struct log_limit *lim, int64_t now_ns, enum log_level lv, const char *fmt, ...)
{
	va_list ap;

	lim->count++;
	if (lim->primed && now_ns - lim->last_ns < LOG_LIMIT_NS)
		return;
	va_start(ap, fmt);
	emit(lv, lim->count, fmt, ap);
	va_end(ap);
	lim->count = 0;
	lim->last_ns = now_ns;
	lim->primed = true;
}
