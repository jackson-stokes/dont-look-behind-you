/* Periodic soak report timer (DESIGN.md §6 48 h soak). dlby-super logs liveness,
 * RSS and CPU for every child; fuse-hub, store and alert log their event counts.
 * All of them run one of these from their main loop.
 *
 * $DLBY_REPORT_S sets the period in whole seconds, 1..REPORT_MAX_S, default
 * REPORT_DEFAULT_S. The soak needs a report at least hourly, so longer periods
 * are refused, not clamped. Deadlines are absolute (next += period), so reports
 * don't drift; after a stall longer than one period the next is one period on.
 *
 * Final state: on exit every reporter logs one last report with final=1, so the
 * soak log ends with the final state of every process. dlby-super logs its
 * final report when shutdown begins, while the children can still be read, and
 * a report_end line (uptime, total restarts) just before its exit line; the
 * others log theirs just before their exit line.
 */
#ifndef DLBY_REPORT_H
#define DLBY_REPORT_H

#include <stdbool.h>
#include <stdint.h>

#define REPORT_ENV       "DLBY_REPORT_S"
#define REPORT_DEFAULT_S 1800
#define REPORT_MAX_S     3600

struct report {
	int64_t  period_ns;
	int64_t  next_ns;
	uint32_t n;		/* reports made so far, including a final one */
};

/* NULL or "" -> the default. Returns 0, or -1 with EINVAL. */
int report_parse_s(const char *s, int64_t *period_ns);

/* Reads $DLBY_REPORT_S. Returns 0, or -1 with EINVAL (log it and exit 78). */
int report_init(struct report *r, int64_t now_ns);

/* True once per period; advances the deadline and counts the report. */
bool report_due(struct report *r, int64_t now_ns);

/* Counts the final report; call once, just before logging it. */
void report_final(struct report *r);

#endif
