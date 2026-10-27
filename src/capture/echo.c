#include "echo.h"

enum echo_result echo_classify(const struct echo_edge *e, size_t n, bool high_now,
			       struct echo_ping *out)
{
	int64_t width;
	size_t i, j;

	/* A leading falling edge is the tail of an earlier pulse: skip it. */
	for (i = 0; i < n && !e[i].rising; i++)
		;
	if (i == n)
		return high_now ? ECHO_STUCK : ECHO_NONE;
	for (j = i + 1; j < n && e[j].rising; j++)
		;
	if (j == n)
		return ECHO_STUCK;

	width = e[j].ts_ns - e[i].ts_ns;
	if (width < 0)
		width = 0;	/* not on one clock; never seen, but don't wrap */
	out->rise_ns = e[i].ts_ns;
	out->width_ns = width > UINT32_MAX ? UINT32_MAX : (uint32_t)width;
	return width > ECHO_MAX_NS ? ECHO_FAR : ECHO_OK;
}

const char *echo_result_name(enum echo_result r)
{
	switch (r) {
	case ECHO_OK:    return "ok";
	case ECHO_FAR:   return "far";
	case ECHO_NONE:  return "no_echo";
	case ECHO_STUCK: return "stuck_high";
	}
	return "unknown";
}
