/* Classifies one HC-SR04 ping for cap-sonar from the kernel-stamped edges seen
 * on ECHO after the trigger (DESIGN.md §2 C). Pure: no I/O, so it is unit-tested
 * directly.
 *
 *   ECHO_OK     rise then fall, width <= ECHO_MAX_NS: send echo_ns
 *   ECHO_FAR    rise then fall, wider: sensor alive, nothing inside 4.3 m
 *               (SONAR_NO_ECHO)
 *   ECHO_NONE   no rising edge and the line is low: sensor not answering
 *   ECHO_STUCK  still high at the end of the ping: stuck high (> 60 ms)
 *
 * fuse-hub counts any SONAR message as "alive", so cap-sonar sends nothing for
 * NONE and STUCK; the hub's 8-ping staleness then marks the sonar lost.
 */
#ifndef DLBY_ECHO_H
#define DLBY_ECHO_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define ECHO_MAX_NS 25000000LL	/* 2 * 4.3 m / 343 m/s */

enum echo_result {
	ECHO_OK,
	ECHO_FAR,
	ECHO_NONE,
	ECHO_STUCK,
};

struct echo_edge {
	int64_t ts_ns;		/* kernel timestamp, CLOCK_MONOTONIC */
	bool    rising;
};

struct echo_ping {
	int64_t  rise_ns;	/* valid for OK and FAR */
	uint32_t width_ns;	/* valid for OK and FAR */
};

/* e: the edges since the trigger, oldest first. high_now: ECHO's level at the
 * end of the ping. */
enum echo_result echo_classify(const struct echo_edge *e, size_t n, bool high_now,
			       struct echo_ping *out);

const char *echo_result_name(enum echo_result r);

#endif
