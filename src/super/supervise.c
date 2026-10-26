#include "supervise.h"

#include <string.h>
#include <sys/wait.h>
#include <sysexits.h>

void sup_init(struct child_sm *c)
{
	memset(c, 0, sizeof(*c));
	c->state = CH_IDLE;
	c->backoff_ms = SUP_BACKOFF_MIN_MS;
}

void sup_on_start(struct child_sm *c, int64_t now_ns)
{
	c->state = CH_RUNNING;
	c->start_ns = now_ns;
	c->last_hb_ns = now_ns;
	c->got_hb = false;
	c->killed_by_us = false;
}

bool sup_on_hb(struct child_sm *c, int64_t now_ns)
{
	bool first = !c->got_hb;

	c->got_hb = true;
	c->last_hb_ns = now_ns;
	return first;
}

enum sup_exit sup_on_exit(struct child_sm *c, int status, int64_t now_ns)
{
	if (now_ns - c->start_ns >= SUP_HEALTHY_NS)
		c->backoff_ms = SUP_BACKOFF_MIN_MS;
	if (!c->killed_by_us && WIFEXITED(status)) {
		if (WEXITSTATUS(status) == 0) {
			c->state = CH_STOPPED;
			return SUP_EXIT_STOPPED;
		}
		if (WEXITSTATUS(status) == EX_CONFIG) {
			c->state = CH_DISABLED;
			return SUP_EXIT_DISABLED;
		}
	}
	c->delay_ms = c->backoff_ms;
	c->restart_at_ns = now_ns + (int64_t)c->backoff_ms * 1000000;
	c->backoff_ms = c->backoff_ms * 2 > SUP_BACKOFF_MAX_MS ? SUP_BACKOFF_MAX_MS : c->backoff_ms * 2;
	c->state = CH_WAIT_RESTART;
	return SUP_EXIT_RESTART;
}

static int64_t hb_deadline(const struct child_sm *c)
{
	return c->got_hb ? c->last_hb_ns + SUP_HB_SILENCE_NS : c->start_ns + SUP_HB_FIRST_NS;
}

enum sup_action sup_tick(struct child_sm *c, int64_t now_ns)
{
	switch (c->state) {
	case CH_IDLE:
		return SUP_START;
	case CH_WAIT_RESTART:
		return now_ns >= c->restart_at_ns ? SUP_START : SUP_NONE;
	case CH_RUNNING:
		if (now_ns <= hb_deadline(c))
			return SUP_NONE;
		c->state = CH_TERMINATING;
		c->killed_by_us = true;
		c->kill_at_ns = now_ns + SUP_KILL_GRACE_NS;
		return SUP_TERM;
	case CH_TERMINATING:
		if (now_ns < c->kill_at_ns)
			return SUP_NONE;
		c->state = CH_KILLING;
		return SUP_KILL;
	default:
		return SUP_NONE;
	}
}

int64_t sup_deadline(const struct child_sm *c)
{
	switch (c->state) {
	case CH_IDLE:
		return INT64_MIN;
	case CH_WAIT_RESTART:
		return c->restart_at_ns;
	case CH_RUNNING:
		return hb_deadline(c) + 1;
	case CH_TERMINATING:
		return c->kill_at_ns;
	default:
		return INT64_MAX;
	}
}
