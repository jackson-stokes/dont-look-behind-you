#include "report.h"

#include <errno.h>
#include <stdlib.h>

#define SEC_NS 1000000000LL

int report_parse_s(const char *s, int64_t *period_ns)
{
	char *end;
	long v;

	if (!s || !*s) {
		*period_ns = REPORT_DEFAULT_S * SEC_NS;
		return 0;
	}
	errno = 0;
	v = strtol(s, &end, 10);
	if (errno || *end || v < 1 || v > REPORT_MAX_S) {
		errno = EINVAL;
		return -1;
	}
	*period_ns = v * SEC_NS;
	return 0;
}

int report_init(struct report *r, int64_t now_ns)
{
	r->n = 0;
	if (report_parse_s(getenv(REPORT_ENV), &r->period_ns) < 0)
		return -1;
	r->next_ns = now_ns + r->period_ns;
	return 0;
}

bool report_due(struct report *r, int64_t now_ns)
{
	if (now_ns < r->next_ns)
		return false;
	r->next_ns += r->period_ns;
	if (r->next_ns <= now_ns)
		r->next_ns = now_ns + r->period_ns;
	r->n++;
	return true;
}

void report_final(struct report *r)
{
	r->n++;
}
