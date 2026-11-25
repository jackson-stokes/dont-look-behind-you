#include "check.h"
#include "msg.h"
#include "pattern.h"

#define MS 1000000LL

static void test_clear(void)
{
	CHECK(pattern_led(ST_CLEAR, 0) && pattern_led(ST_CLEAR, 99 * MS));
	CHECK(!pattern_led(ST_CLEAR, 100 * MS) && !pattern_led(ST_CLEAR, 1999 * MS));
	CHECK(pattern_led(ST_CLEAR, 2000 * MS));			/* every 2 s */
	CHECK(!pattern_buzz(ST_CLEAR, 0) && !pattern_buzz(ST_CLEAR, 5000 * MS));
}

static void test_approaching_in_zone(void)
{
	int64_t t;

	for (t = 0; t < 3000 * MS; t += 7 * MS) {
		CHECK(pattern_led(ST_APPROACHING, t) && !pattern_buzz(ST_APPROACHING, t));
		CHECK(pattern_led(ST_IN_ZONE, t));			/* solid */
	}
	/* 4 Hz beeps: 125 ms on, 125 ms off. */
	CHECK(pattern_buzz(ST_IN_ZONE, 0) && pattern_buzz(ST_IN_ZONE, 124 * MS));
	CHECK(!pattern_buzz(ST_IN_ZONE, 125 * MS) && !pattern_buzz(ST_IN_ZONE, 249 * MS));
	CHECK(pattern_buzz(ST_IN_ZONE, 250 * MS) && pattern_buzz(ST_IN_ZONE, 1000 * MS));
	CHECK(pattern_next_edge(ST_APPROACHING, 0) == INT64_MAX);	/* never changes */
}

static void test_degraded(void)
{
	/* DEG_CLEAR: two blinks every 2 s, one chirp on entry only. */
	CHECK(pattern_led(ST_DEG_CLEAR, 50 * MS) && !pattern_led(ST_DEG_CLEAR, 200 * MS));
	CHECK(pattern_led(ST_DEG_CLEAR, 350 * MS) && !pattern_led(ST_DEG_CLEAR, 400 * MS));
	CHECK(pattern_led(ST_DEG_CLEAR, 2350 * MS));
	CHECK(pattern_buzz(ST_DEG_CLEAR, 0) && pattern_buzz(ST_DEG_CLEAR, 79 * MS));
	CHECK(!pattern_buzz(ST_DEG_CLEAR, 80 * MS));
	CHECK(!pattern_buzz(ST_DEG_CLEAR, 2000 * MS) && !pattern_buzz(ST_DEG_CLEAR, 30000 * MS));
	/* DEG_APPROACHING / DEG_IN_ZONE: 1 Hz flash; DEG_IN_ZONE 1 Hz beeps. */
	CHECK(pattern_led(ST_DEG_APPROACHING, 499 * MS) && !pattern_led(ST_DEG_APPROACHING, 500 * MS));
	CHECK(pattern_led(ST_DEG_APPROACHING, 1000 * MS) && !pattern_buzz(ST_DEG_APPROACHING, 0));
	CHECK(pattern_led(ST_DEG_IN_ZONE, 0) && !pattern_led(ST_DEG_IN_ZONE, 600 * MS));
	CHECK(pattern_buzz(ST_DEG_IN_ZONE, 249 * MS) && !pattern_buzz(ST_DEG_IN_ZONE, 250 * MS));
	CHECK(pattern_buzz(ST_DEG_IN_ZONE, 1100 * MS));
}

/* BLIND and HUB_LOST share a pattern: 4 Hz blink, two chirps every 30 s. */
static void test_not_protected(void)
{
	unsigned s[2] = { ST_BLIND, PAT_HUB_LOST };
	int i;

	for (i = 0; i < 2; i++) {
		CHECK(pattern_led(s[i], 0) && !pattern_led(s[i], 125 * MS) && pattern_led(s[i], 250 * MS));
		CHECK(pattern_buzz(s[i], 0) && !pattern_buzz(s[i], 80 * MS));
		CHECK(pattern_buzz(s[i], 160 * MS) && !pattern_buzz(s[i], 240 * MS));
		CHECK(!pattern_buzz(s[i], 1000 * MS) && !pattern_buzz(s[i], 29999 * MS));
		CHECK(pattern_buzz(s[i], 30000 * MS) && pattern_buzz(s[i], 30170 * MS));
		CHECK(pattern_next_edge(s[i], 300 * MS) == 375 * MS);	/* LED edge first */
	}
}

/* Every pattern starts in its on-phase, so a state change writes at once. */
static void test_starts_on(void)
{
	unsigned s;

	for (s = 0; s < PAT_NSTATES; s++)
		CHECK(pattern_led(s, 0));
	CHECK(!pattern_led(99, 0) && !pattern_buzz(99, 0));	/* unknown: dark */
	CHECK(pattern_next_edge(99, 0) == INT64_MAX);
	CHECK(pattern_led(ST_CLEAR, -5 * MS));			/* t < 0 is 0 */
}

/* pattern_next_edge must name every change and only changes: a 1 ms scan over
 * 32 s (past the 30 s chirp period) for every state. */
static void test_next_edge_matches_scan(void)
{
	unsigned s;
	int64_t t, next;
	bool led, buzz, l2, b2;

	for (s = 0; s < PAT_NSTATES; s++) {
		next = pattern_next_edge(s, 0);
		led = pattern_led(s, 0);
		buzz = pattern_buzz(s, 0);
		for (t = MS; t <= 32000 * MS; t += MS) {
			l2 = pattern_led(s, t);
			b2 = pattern_buzz(s, t);
			if (l2 != led || b2 != buzz) {
				if (t != next)
					printf("  state %s: change at %lld ms, predicted %lld\n",
					       pattern_state_name(s), (long long)(t / MS),
					       (long long)(next / MS));
				CHECK(t == next);
				next = pattern_next_edge(s, t);
				CHECK(next > t);
			} else {
				CHECK(t < next);	/* no change predicted that didn't happen */
			}
			led = l2;
			buzz = b2;
		}
	}
}

static void test_buzzer_side(void)
{
	CHECK(pattern_buzz_side(ST_CLEAR, ST_CLEAR) == 0);		/* tie -> right */
	CHECK(pattern_buzz_side(ST_CLEAR, ST_BLIND) == 1);		/* left unwatched chirps */
	CHECK(pattern_buzz_side(ST_IN_ZONE, ST_BLIND) == 0);		/* a warning wins */
	CHECK(pattern_buzz_side(ST_DEG_CLEAR, ST_APPROACHING) == 0);	/* entry chirp > silent */
	CHECK(pattern_buzz_side(ST_DEG_IN_ZONE, PAT_HUB_LOST) == 0);
	CHECK(pattern_buzz_side(PAT_HUB_LOST, PAT_HUB_LOST) == 0);
	CHECK(pattern_buzz_rank(ST_IN_ZONE) > pattern_buzz_rank(ST_DEG_IN_ZONE));
	CHECK(pattern_buzz_rank(ST_DEG_IN_ZONE) > pattern_buzz_rank(ST_BLIND));
	CHECK(pattern_buzz_rank(ST_BLIND) == pattern_buzz_rank(PAT_HUB_LOST));
	CHECK(pattern_buzz_rank(ST_BLIND) > pattern_buzz_rank(ST_DEG_CLEAR));
	CHECK(pattern_buzz_rank(ST_DEG_CLEAR) > pattern_buzz_rank(ST_APPROACHING));
	CHECK(pattern_buzz_rank(ST_APPROACHING) == 0 && pattern_buzz_rank(ST_CLEAR) == 0);
}

static void test_names(void)
{
	CHECK(strcmp(pattern_state_name(PAT_HUB_LOST), "HUB_LOST") == 0);
	CHECK(strcmp(pattern_state_name(ST_IN_ZONE), "IN_ZONE") == 0);
	CHECK(strcmp(pattern_state_name(PAT_NSTATES), "UNKNOWN") == 0);
}

int main(void)
{
	RUN(test_clear);
	RUN(test_approaching_in_zone);
	RUN(test_degraded);
	RUN(test_not_protected);
	RUN(test_starts_on);
	RUN(test_next_edge_matches_scan);
	RUN(test_buzzer_side);
	RUN(test_names);
	return 0;
}
