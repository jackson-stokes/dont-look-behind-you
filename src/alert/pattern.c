#include "pattern.h"

#include "msg.h"

#include <assert.h>

#define MS 1000000LL

static_assert(PAT_HUB_LOST == ST_DEG_IN_ZONE + 1, "HUB_LOST follows the fusion states");

/* One output's waveform: solid, or on-windows [start, end) repeating every
 * period (period 0: once, from entry). Windows never touch or fill the period,
 * so every window boundary is a real change. */
struct wave {
	bool    solid;
	int64_t period;
	int     n;
	int64_t on[2][2];
};

struct pat {
	struct wave led, buzz;
};

#define SILENT        { .solid = false, .n = 0 }
#define SOLID         { .solid = true }
#define BLINK_4HZ     { .period = 250 * MS, .n = 1, .on = { { 0, 125 * MS } } }
#define FLASH_1HZ     { .period = 1000 * MS, .n = 1, .on = { { 0, 500 * MS } } }

static const struct pat pats[PAT_NSTATES] = {
	[ST_CLEAR] = {
		.led  = { .period = 2000 * MS, .n = 1, .on = { { 0, 100 * MS } } },
		.buzz = SILENT,
	},
	[ST_APPROACHING] = { .led = SOLID, .buzz = SILENT },
	[ST_IN_ZONE]     = { .led = SOLID, .buzz = BLINK_4HZ },
	[ST_DEG_CLEAR] = {
		.led  = { .period = 2000 * MS, .n = 2,
			  .on = { { 0, 100 * MS }, { 300 * MS, 400 * MS } } },
		.buzz = { .period = 0, .n = 1, .on = { { 0, 80 * MS } } },
	},
	[ST_DEG_APPROACHING] = { .led = FLASH_1HZ, .buzz = SILENT },
	[ST_DEG_IN_ZONE] = {
		.led  = FLASH_1HZ,
		.buzz = { .period = 1000 * MS, .n = 1, .on = { { 0, 250 * MS } } },
	},
	[ST_BLIND] = {
		.led  = BLINK_4HZ,
		.buzz = { .period = 30000 * MS, .n = 2,
			  .on = { { 0, 80 * MS }, { 160 * MS, 240 * MS } } },
	},
	[PAT_HUB_LOST] = {
		.led  = BLINK_4HZ,
		.buzz = { .period = 30000 * MS, .n = 2,
			  .on = { { 0, 80 * MS }, { 160 * MS, 240 * MS } } },
	},
};

static bool wave_level(const struct wave *w, int64_t t)
{
	int64_t ph;
	int i;

	if (w->solid)
		return true;
	if (t < 0)
		t = 0;
	ph = w->period ? t % w->period : t;
	for (i = 0; i < w->n; i++)
		if (ph >= w->on[i][0] && ph < w->on[i][1])
			return true;
	return false;
}

/* Smallest window boundary strictly after t. */
static int64_t wave_next(const struct wave *w, int64_t t)
{
	int64_t best = INT64_MAX, base, b;
	int i, j, k;

	if (w->solid || w->n == 0)
		return INT64_MAX;
	if (t < 0)
		t = 0;
	if (w->period == 0) {
		for (i = 0; i < w->n; i++)
			for (j = 0; j < 2; j++)
				if (w->on[i][j] > t && w->on[i][j] < best)
					best = w->on[i][j];
		return best;
	}
	base = t - t % w->period;
	for (k = 0; k < 2; k++)		/* this period and the next */
		for (i = 0; i < w->n; i++)
			for (j = 0; j < 2; j++) {
				b = base + k * w->period + w->on[i][j];
				if (b > t && b < best)
					best = b;
			}
	return best;
}

static const struct pat *get(unsigned state)
{
	return state < PAT_NSTATES ? &pats[state] : NULL;
}

bool pattern_led(unsigned state, int64_t t)
{
	const struct pat *p = get(state);

	return p && wave_level(&p->led, t);
}

bool pattern_buzz(unsigned state, int64_t t)
{
	const struct pat *p = get(state);

	return p && wave_level(&p->buzz, t);
}

int64_t pattern_next_edge(unsigned state, int64_t t)
{
	const struct pat *p = get(state);
	int64_t a, b;

	if (!p)
		return INT64_MAX;
	a = wave_next(&p->led, t);
	b = wave_next(&p->buzz, t);
	return a < b ? a : b;
}

int pattern_buzz_rank(unsigned state)
{
	switch (state) {
	case ST_IN_ZONE:     return 4;
	case ST_DEG_IN_ZONE: return 3;
	case ST_BLIND:
	case PAT_HUB_LOST:   return 2;
	case ST_DEG_CLEAR:   return 1;
	default:             return 0;
	}
}

int pattern_buzz_side(unsigned right, unsigned left)
{
	return pattern_buzz_rank(left) > pattern_buzz_rank(right) ? 1 : 0;
}

const char *pattern_state_name(unsigned state)
{
	static const char *const names[PAT_NSTATES] = {
		[ST_BLIND]           = "BLIND",
		[ST_CLEAR]           = "CLEAR",
		[ST_APPROACHING]     = "APPROACHING",
		[ST_IN_ZONE]         = "IN_ZONE",
		[ST_DEG_CLEAR]       = "DEG_CLEAR",
		[ST_DEG_APPROACHING] = "DEG_APPROACHING",
		[ST_DEG_IN_ZONE]     = "DEG_IN_ZONE",
		[PAT_HUB_LOST]       = "HUB_LOST",
	};

	return state < PAT_NSTATES ? names[state] : "UNKNOWN";
}
