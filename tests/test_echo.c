#include "check.h"
#include "echo.h"

#define US 1000LL
#define MS 1000000LL

/* The Pi smoke test: 517.111786550 rising, 517.122034466 falling = 176.7 cm. */
static void test_wall_reading(void)
{
	const struct echo_edge e[] = {
		{ 517111786550LL, true },
		{ 517122034466LL, false },
	};
	struct echo_ping p;

	CHECK(echo_classify(e, 2, false, &p) == ECHO_OK);
	CHECK(p.rise_ns == 517111786550LL);
	CHECK(p.width_ns == 10247916u);
	CHECK(p.width_ns / 58000u == 176u);	/* 58 us per cm */
}

static void test_range_boundary(void)
{
	const struct echo_edge at[] = { { 0, true }, { ECHO_MAX_NS, false } };
	const struct echo_edge past[] = { { 0, true }, { ECHO_MAX_NS + 1, false } };
	struct echo_ping p;

	CHECK(echo_classify(at, 2, false, &p) == ECHO_OK);
	CHECK(echo_classify(past, 2, false, &p) == ECHO_FAR);
	CHECK(p.width_ns == ECHO_MAX_NS + 1);
}

/* Clone sensors answer "nothing in range" with a ~38 ms pulse. */
static void test_no_target_pulse_is_far(void)
{
	const struct echo_edge e[] = { { 500 * US, true }, { 38 * MS, false } };
	struct echo_ping p;

	CHECK(echo_classify(e, 2, false, &p) == ECHO_FAR);
}

static void test_no_edges(void)
{
	struct echo_ping p;

	CHECK(echo_classify(NULL, 0, false, &p) == ECHO_NONE);
	CHECK(echo_classify(NULL, 0, true, &p) == ECHO_STUCK);
}

static void test_rise_without_fall_is_stuck(void)
{
	const struct echo_edge e[] = { { 1 * MS, true } };
	struct echo_ping p;

	CHECK(echo_classify(e, 1, true, &p) == ECHO_STUCK);
}

static void test_leading_fall_skipped(void)
{
	const struct echo_edge e[] = {
		{ 1 * MS, false },	/* tail of the previous pulse */
		{ 2 * MS, true },
		{ 3 * MS, false },
	};
	struct echo_ping p;

	CHECK(echo_classify(e, 3, false, &p) == ECHO_OK);
	CHECK(p.rise_ns == 2 * MS && p.width_ns == 1 * MS);
}

static void test_only_falls_is_none(void)
{
	const struct echo_edge e[] = { { 1 * MS, false } };
	struct echo_ping p;

	CHECK(echo_classify(e, 1, false, &p) == ECHO_NONE);
}

/* A glitch: the first full pulse wins. */
static void test_first_pulse_wins(void)
{
	const struct echo_edge e[] = {
		{ 1 * MS, true }, { 2 * MS, false },
		{ 5 * MS, true }, { 9 * MS, false },
	};
	struct echo_ping p;

	CHECK(echo_classify(e, 4, false, &p) == ECHO_OK);
	CHECK(p.rise_ns == 1 * MS && p.width_ns == 1 * MS);
}

static void test_names(void)
{
	CHECK(strcmp(echo_result_name(ECHO_NONE), "no_echo") == 0);
	CHECK(strcmp(echo_result_name(ECHO_STUCK), "stuck_high") == 0);
}

int main(void)
{
	RUN(test_wall_reading);
	RUN(test_range_boundary);
	RUN(test_no_target_pulse_is_far);
	RUN(test_no_edges);
	RUN(test_rise_without_fall_is_stuck);
	RUN(test_leading_fall_skipped);
	RUN(test_only_falls_is_none);
	RUN(test_first_pulse_wins);
	RUN(test_names);
	return 0;
}
