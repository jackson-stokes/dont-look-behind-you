#include "check.h"
#include "gpio_out.h"

#include <sysexits.h>

static void clear_env(void)
{
	CHECK(unsetenv(ALERT_CHIP_ENV) == 0 && unsetenv(ALERT_LED_RIGHT_ENV) == 0 &&
	      unsetenv(ALERT_LED_LEFT_ENV) == 0 && unsetenv(ALERT_BUZZ_ENV) == 0);
}

static void test_defaults(void)
{
	struct alert_pins p;
	char s[32];

	clear_env();
	CHECK(alert_pins_from_env(&p) == 0);
	CHECK(!p.none && strcmp(p.chip, ALERT_CHIP_DEFAULT) == 0);
	CHECK(p.line[AL_LED_RIGHT] == 17 && p.line[AL_LED_LEFT] == 27 && p.line[AL_BUZZ] == 22);
	alert_pins_str(&p, s, sizeof(s));
	CHECK(strcmp(s, "17,27,22") == 0);
}

static void test_none_and_overrides(void)
{
	struct alert_pins p;
	char s[32];

	clear_env();
	CHECK(setenv(ALERT_CHIP_ENV, "none", 1) == 0);
	CHECK(alert_pins_from_env(&p) == 0 && p.none);
	alert_pins_str(&p, s, sizeof(s));
	CHECK(strcmp(s, "none") == 0);
	errno = 0;
	CHECK(gpio_out_open(&p, "test") == -1 && errno == EINVAL);	/* never opened */
	CHECK(setenv(ALERT_CHIP_ENV, "/dev/gpiochip4", 1) == 0);
	CHECK(setenv(ALERT_LED_LEFT_ENV, "26", 1) == 0);
	CHECK(alert_pins_from_env(&p) == 0 && !p.none && strcmp(p.chip, "/dev/gpiochip4") == 0);
	CHECK(p.line[AL_LED_LEFT] == 26);
	clear_env();
}

static void test_bad_settings(void)
{
	static const char *const bad[] = { "abc", "-1", "512", "17x", "" };
	struct alert_pins p;
	size_t i;

	for (i = 0; i < sizeof(bad) / sizeof(bad[0]); i++) {
		clear_env();
		CHECK(setenv(ALERT_BUZZ_ENV, bad[i], 1) == 0);
		errno = 0;
		/* "" means the default, like an unset variable. */
		CHECK(alert_pins_from_env(&p) == (bad[i][0] ? -1 : 0));
		if (bad[i][0])
			CHECK(errno == EINVAL);
	}
	/* Two roles on one line. */
	clear_env();
	CHECK(setenv(ALERT_LED_LEFT_ENV, "17", 1) == 0);
	CHECK(alert_pins_from_env(&p) == -1 && errno == EINVAL);
	clear_env();
	CHECK(setenv(ALERT_BUZZ_ENV, "27", 1) == 0);
	CHECK(alert_pins_from_env(&p) == -1);
	/* A chip path that doesn't fit. */
	clear_env();
	CHECK(setenv(ALERT_CHIP_ENV,
		     "/dev/a-very-long-path-that-is-much-longer-than-the-sixty-four-byte-buffer", 1) == 0);
	CHECK(alert_pins_from_env(&p) == -1 && errno == EINVAL);
	clear_env();
}

static void test_open_missing_chip(void)
{
	struct alert_pins p;

	clear_env();
	CHECK(setenv(ALERT_CHIP_ENV, "/nonexistent/gpiochip0", 1) == 0);
	CHECK(alert_pins_from_env(&p) == 0);
	errno = 0;
	CHECK(gpio_out_open(&p, "test") == -1 && errno == ENOENT);
	CHECK(gpio_out_exit_code(ENOENT) == EX_CONFIG);
	CHECK(gpio_out_exit_code(EACCES) == EX_CONFIG);
	CHECK(gpio_out_exit_code(EBUSY) == EX_UNAVAILABLE);
	CHECK(gpio_out_exit_code(EIO) == EX_UNAVAILABLE);
	clear_env();
}

int main(void)
{
	RUN(test_defaults);
	RUN(test_none_and_overrides);
	RUN(test_bad_settings);
	RUN(test_open_missing_chip);
	return 0;
}
