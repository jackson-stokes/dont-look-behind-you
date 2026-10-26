#include "check.h"
#include "supervise.h"

#include <signal.h>
#include <sys/wait.h>
#include <sysexits.h>

#define MS  1000000LL
#define SEC 1000000000LL

static int exited(int code)
{
	return W_EXITCODE(code, 0);
}

static int killed(int sig)
{
	return W_EXITCODE(0, sig);
}

static void test_first_start_is_immediate(void)
{
	struct child_sm c;

	sup_init(&c);
	CHECK(sup_tick(&c, 0) == SUP_START);
	CHECK(sup_deadline(&c) == INT64_MIN);
	sup_on_start(&c, 0);
	CHECK(c.state == CH_RUNNING && sup_tick(&c, 0) == SUP_NONE);
}

static void test_backoff_doubles_to_cap(void)
{
	static const int want[] = { 100, 200, 400, 800, 1600, 3200, 6400, 12800, 25600, 30000, 30000 };
	struct child_sm c;
	int64_t t = 0;
	size_t i;

	sup_init(&c);
	for (i = 0; i < sizeof(want) / sizeof(want[0]); i++) {
		sup_on_start(&c, t);
		t += 1 * SEC;				/* short-lived: no reset */
		CHECK(sup_on_exit(&c, killed(SIGSEGV), t) == SUP_EXIT_RESTART);
		CHECK(c.delay_ms == want[i]);
		CHECK(c.restart_at_ns == t + want[i] * MS);
		CHECK(sup_tick(&c, c.restart_at_ns - 1) == SUP_NONE);
		CHECK(sup_tick(&c, c.restart_at_ns) == SUP_START);
		t = c.restart_at_ns;
	}
}

static void test_backoff_resets_after_60s_healthy(void)
{
	struct child_sm c;

	sup_init(&c);
	sup_on_start(&c, 0);
	sup_on_exit(&c, exited(70), 1 * SEC);		/* 100 */
	sup_on_start(&c, 2 * SEC);
	sup_on_exit(&c, exited(70), 3 * SEC);		/* 200 */
	sup_on_start(&c, 4 * SEC);
	sup_on_exit(&c, exited(70), 4 * SEC + 59 * SEC);	/* ran 59 s: no reset */
	CHECK(c.delay_ms == 400);
	sup_on_start(&c, 100 * SEC);
	sup_on_exit(&c, exited(70), 160 * SEC);	/* ran 60 s: reset */
	CHECK(c.delay_ms == 100);
}

static void test_exit_code_table(void)
{
	struct child_sm c;

	sup_init(&c);
	sup_on_start(&c, 0);
	CHECK(sup_on_exit(&c, exited(0), SEC) == SUP_EXIT_STOPPED && c.state == CH_STOPPED);
	CHECK(sup_tick(&c, 100 * SEC) == SUP_NONE);

	sup_init(&c);
	sup_on_start(&c, 0);
	CHECK(sup_on_exit(&c, exited(EX_CONFIG), SEC) == SUP_EXIT_DISABLED && c.state == CH_DISABLED);
	CHECK(sup_tick(&c, 100 * SEC) == SUP_NONE && sup_deadline(&c) == INT64_MAX);

	sup_init(&c);
	sup_on_start(&c, 0);
	CHECK(sup_on_exit(&c, exited(EX_UNAVAILABLE), SEC) == SUP_EXIT_RESTART);
	sup_on_start(&c, 2 * SEC);
	CHECK(sup_on_exit(&c, exited(EX_SOFTWARE), 3 * SEC) == SUP_EXIT_RESTART);
	sup_on_start(&c, 4 * SEC);
	CHECK(sup_on_exit(&c, exited(1), 5 * SEC) == SUP_EXIT_RESTART);	/* unlisted code */
	sup_on_start(&c, 6 * SEC);
	CHECK(sup_on_exit(&c, killed(SIGKILL), 7 * SEC) == SUP_EXIT_RESTART);
	CHECK(c.state == CH_WAIT_RESTART);
}

static void test_exit_0_after_our_kill_restarts(void)
{
	struct child_sm c;

	sup_init(&c);
	sup_on_start(&c, 0);
	CHECK(sup_tick(&c, SUP_HB_FIRST_NS + 1) == SUP_TERM);	/* never heartbeat */
	CHECK(c.killed_by_us);
	CHECK(sup_on_exit(&c, exited(0), SUP_HB_FIRST_NS + 2) == SUP_EXIT_RESTART);
	sup_on_start(&c, 20 * SEC);
	CHECK(!c.killed_by_us);
	CHECK(sup_on_exit(&c, exited(EX_CONFIG), 21 * SEC) == SUP_EXIT_DISABLED);
}

static void test_first_heartbeat_deadline(void)
{
	struct child_sm c;

	sup_init(&c);
	sup_on_start(&c, 0);
	CHECK(sup_deadline(&c) == SUP_HB_FIRST_NS + 1);
	CHECK(sup_tick(&c, SUP_HB_FIRST_NS) == SUP_NONE);
	CHECK(sup_tick(&c, SUP_HB_FIRST_NS + 1) == SUP_TERM);

	sup_init(&c);
	sup_on_start(&c, 0);
	CHECK(sup_on_hb(&c, 9 * SEC));
	CHECK(!sup_on_hb(&c, 9 * SEC + 1));		/* only the first is first */
	CHECK(sup_tick(&c, SUP_HB_FIRST_NS + 1) == SUP_NONE);
}

static void test_silence_term_then_kill(void)
{
	struct child_sm c;
	int64_t last = 3 * SEC;

	sup_init(&c);
	sup_on_start(&c, 0);
	sup_on_hb(&c, last);
	CHECK(sup_tick(&c, last + SUP_HB_SILENCE_NS) == SUP_NONE);
	CHECK(sup_tick(&c, last + SUP_HB_SILENCE_NS + 1) == SUP_TERM);
	CHECK(c.state == CH_TERMINATING);
	CHECK(sup_deadline(&c) == last + SUP_HB_SILENCE_NS + 1 + SUP_KILL_GRACE_NS);

	/* a late heartbeat doesn't call off the kill */
	sup_on_hb(&c, last + SUP_HB_SILENCE_NS + 2);
	CHECK(sup_tick(&c, last + SUP_HB_SILENCE_NS + SUP_KILL_GRACE_NS) == SUP_NONE);
	CHECK(sup_tick(&c, last + SUP_HB_SILENCE_NS + 1 + SUP_KILL_GRACE_NS) == SUP_KILL);
	CHECK(c.state == CH_KILLING && sup_tick(&c, 100 * SEC) == SUP_NONE);
	CHECK(sup_on_exit(&c, killed(SIGKILL), 100 * SEC) == SUP_EXIT_RESTART);
}

int main(void)
{
	RUN(test_first_start_is_immediate);
	RUN(test_backoff_doubles_to_cap);
	RUN(test_backoff_resets_after_60s_healthy);
	RUN(test_exit_code_table);
	RUN(test_exit_0_after_our_kill_restarts);
	RUN(test_first_heartbeat_deadline);
	RUN(test_silence_term_then_kill);
	return 0;
}
