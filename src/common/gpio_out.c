#include "gpio_out.h"

#include <errno.h>
#include <fcntl.h>
#include <linux/gpio.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sysexits.h>
#include <unistd.h>

static int parse_line(const char *env, unsigned dflt, unsigned *out)
{
	const char *s = getenv(env);
	char *end;
	long v;

	if (!s || !*s) {
		*out = dflt;
		return 0;
	}
	errno = 0;
	v = strtol(s, &end, 10);
	if (errno || end == s || *end != '\0' || v < 0 || v > 511) {
		errno = EINVAL;
		return -1;
	}
	*out = (unsigned)v;
	return 0;
}

int alert_pins_from_env(struct alert_pins *p)
{
	const char *chip = getenv(ALERT_CHIP_ENV);
	int n;

	memset(p, 0, sizeof(*p));
	if (!chip || !*chip)
		chip = ALERT_CHIP_DEFAULT;
	p->none = strcmp(chip, ALERT_CHIP_NONE) == 0;
	n = snprintf(p->chip, sizeof(p->chip), "%s", chip);
	if (n < 0 || (size_t)n >= sizeof(p->chip))
		goto bad;
	if (parse_line(ALERT_LED_RIGHT_ENV, 17, &p->line[AL_LED_RIGHT]) < 0 ||
	    parse_line(ALERT_LED_LEFT_ENV, 27, &p->line[AL_LED_LEFT]) < 0 ||
	    parse_line(ALERT_BUZZ_ENV, 22, &p->line[AL_BUZZ]) < 0)
		goto bad;
	if (p->line[AL_LED_RIGHT] == p->line[AL_LED_LEFT] ||
	    p->line[AL_LED_RIGHT] == p->line[AL_BUZZ] || p->line[AL_LED_LEFT] == p->line[AL_BUZZ])
		goto bad;
	return 0;
bad:
	errno = EINVAL;
	return -1;
}

void alert_pins_str(const struct alert_pins *p, char *buf, size_t cap)
{
	if (p->none)
		snprintf(buf, cap, "none");
	else
		snprintf(buf, cap, "%u,%u,%u", p->line[AL_LED_RIGHT], p->line[AL_LED_LEFT],
			 p->line[AL_BUZZ]);
}

int gpio_out_open(const struct alert_pins *p, const char *consumer)
{
	struct gpio_v2_line_request req;
	int chip, err, i;

	if (p->none) {
		errno = EINVAL;
		return -1;
	}
	chip = open(p->chip, O_RDWR | O_CLOEXEC);
	if (chip < 0)
		return -1;
	memset(&req, 0, sizeof(req));
	for (i = 0; i < AL_NLINES; i++)
		req.offsets[i] = p->line[i];
	req.num_lines = AL_NLINES;
	strncpy(req.consumer, consumer, sizeof(req.consumer) - 1);
	req.config.flags = GPIO_V2_LINE_FLAG_OUTPUT;
	/* Explicitly low, not left to the default. */
	req.config.num_attrs = 1;
	req.config.attrs[0].attr.id = GPIO_V2_LINE_ATTR_ID_OUTPUT_VALUES;
	req.config.attrs[0].attr.values = 0;
	req.config.attrs[0].mask = (1u << AL_NLINES) - 1;
	if (ioctl(chip, GPIO_V2_GET_LINE_IOCTL, &req) < 0) {
		err = errno;
		(void)close(chip);	/* released even if close reports an error */
		errno = err;
		return -1;
	}
	/* The request fd stands alone (close-on-exec from the kernel). */
	if (close(chip) < 0) {
		err = errno;
		(void)close(req.fd);
		errno = err;
		return -1;
	}
	return req.fd;
}

int gpio_out_set(int fd, unsigned bits)
{
	struct gpio_v2_line_values v = {
		.bits = bits & ((1u << AL_NLINES) - 1),
		.mask = (1u << AL_NLINES) - 1,
	};

	return ioctl(fd, GPIO_V2_LINE_SET_VALUES_IOCTL, &v);
}

int gpio_out_exit_code(int err)
{
	return err == ENOENT || err == EACCES || err == ENOTDIR || err == EINVAL ? EX_CONFIG :
	       EX_UNAVAILABLE;
}
