#include "health.h"

#include <string.h>

static const int64_t loss_ns[SENSOR_N] = {
	[SENSOR_RADAR]      = RADAR_LOSS_NS,
	[SENSOR_SONAR]      = SONAR_LOSS_NS,
	[SENSOR_RADAR_LEFT] = RADAR_LOSS_NS,
};

void health_init(struct health *h)
{
	memset(h, 0, sizeof(*h));
}

bool health_rx(struct health *h, enum sensor s, int64_t now_ns)
{
	bool was_up = h->up[s];

	h->last_rx_ns[s] = now_ns;
	h->up[s] = true;
	return !was_up;
}

bool health_eof(struct health *h, enum sensor s)
{
	bool was_up = h->up[s];

	h->up[s] = false;
	return was_up;
}

unsigned health_tick(struct health *h, int64_t now_ns)
{
	unsigned lost = 0;
	int s;

	for (s = 0; s < SENSOR_N; s++) {
		if (h->up[s] && now_ns - h->last_rx_ns[s] > loss_ns[s]) {
			h->up[s] = false;
			lost |= 1u << s;
		}
	}
	return lost;
}

enum health_state health_state(const struct health *h)
{
	int n = 0, s;

	for (s = 0; s < SENSOR_N; s++)
		n += h->up[s];
	return n == SENSOR_N ? H_OK : n > 0 ? H_DEG : H_BLIND;
}

const char *sensor_name(enum sensor s)
{
	static const char *const names[SENSOR_N] = {
		[SENSOR_RADAR]      = "radar",
		[SENSOR_SONAR]      = "sonar",
		[SENSOR_RADAR_LEFT] = "radar_left",
	};

	return s >= 0 && s < SENSOR_N ? names[s] : "unknown";
}
