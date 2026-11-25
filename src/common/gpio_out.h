/* The driver-alert output lines: one LED per side mirror and the buzzer
 * (DESIGN.md §1, docs/HARDWARE.md). Shared by alert, which drives them, and
 * dlby-super, which forces them low whenever alert isn't running (DESIGN.md §2 E
 * "Stuck outputs") and in `dlby-super --outputs-off` (systemd ExecStopPost).
 *
 *   $DLBY_ALERT_CHIP        GPIO chip (default /dev/gpiochip0, the Pi 5 header);
 *                           "none" = no GPIO at all (containers, tests)
 *   $DLBY_ALERT_LED_RIGHT   right mirror LED line (default 17)
 *   $DLBY_ALERT_LED_LEFT    left mirror LED line (default 27)
 *   $DLBY_ALERT_BUZZ        buzzer line, via the 2N2222 (default 22)
 *
 * GPIO uAPI v2, all three lines in one request, outputs starting low.
 */
#ifndef DLBY_GPIO_OUT_H
#define DLBY_GPIO_OUT_H

#include <stdbool.h>
#include <stddef.h>

#define ALERT_CHIP_ENV      "DLBY_ALERT_CHIP"
#define ALERT_LED_RIGHT_ENV "DLBY_ALERT_LED_RIGHT"
#define ALERT_LED_LEFT_ENV  "DLBY_ALERT_LED_LEFT"
#define ALERT_BUZZ_ENV      "DLBY_ALERT_BUZZ"
#define ALERT_CHIP_DEFAULT  "/dev/gpiochip0"
#define ALERT_CHIP_NONE     "none"

enum alert_line {
	AL_LED_RIGHT = 0,
	AL_LED_LEFT  = 1,
	AL_BUZZ      = 2,
	AL_NLINES,
};

#define AL_BIT(l) (1u << (l))

struct alert_pins {
	char     chip[64];
	bool     none;			/* $DLBY_ALERT_CHIP=none */
	unsigned line[AL_NLINES];	/* enum alert_line */
};

/* Returns 0, or -1 with EINVAL for a bad line number (0..511), two roles on the
 * same line, or a chip path that doesn't fit. */
int alert_pins_from_env(struct alert_pins *p);

/* "17,27,22" (right LED, left LED, buzzer) or "none". */
void alert_pins_str(const struct alert_pins *p, char *buf, size_t cap);

/* Requests every line as an output driven low. Returns the request fd
 * (close-on-exec), or -1 with errno: ENOENT etc. for the chip, EBUSY if a line
 * is held elsewhere, EINVAL if p->none. */
int gpio_out_open(const struct alert_pins *p, const char *consumer);

/* Sets every line: bit AL_BIT(line) high, the rest low. Returns 0 or -1. */
int gpio_out_set(int fd, unsigned bits);

/* Exit code for a failed gpio_out_open: 78 (EX_CONFIG) if restarting can't help
 * (no chip, no access, bad line), else 69 (EX_UNAVAILABLE). */
int gpio_out_exit_code(int err);

#endif
