/* Per-sensor health for fuse-hub (DESIGN.md §1, §3). Pure state: no I/O, time is
 * passed in, so it is unit-tested directly. Also the one home of the sensor
 * timing constants shared by the hub, tools/fake-sensors and the tests.
 */
#ifndef DLBY_HEALTH_H
#define DLBY_HEALTH_H

#include <stdbool.h>
#include <stdint.h>

#define RADAR_PERIOD_NS 100000000LL		/* 10 Hz */
#define SONAR_PERIOD_NS  60000000LL		/* HC-SR04 needs >= 60 ms per ping */
/* No valid frame for 3 s. With no targets the LD2451 sends one empty frame every
 * ~1.4 s (docs/evidence/sensor-check.txt); 3 s covers that plus one missed frame. */
#define RADAR_LOSS_NS   3000000000LL
#define SONAR_LOSS_NS   (8 * SONAR_PERIOD_NS)	/* 8 pings = 480 ms */

enum sensor {
	SENSOR_RADAR      = 0,	/* right radar */
	SENSOR_SONAR      = 1,
	SENSOR_RADAR_LEFT = 2,
	SENSOR_N,
};

/* BLIND: no sensor up. DEG: some up. OK: all up. */
enum health_state {
	H_BLIND,
	H_DEG,
	H_OK,
};

struct health {
	int64_t last_rx_ns[SENSOR_N];
	bool    up[SENSOR_N];
};

void health_init(struct health *h);

/* A valid message from s arrived. Returns true if s just came up. */
bool health_rx(struct health *h, enum sensor s, int64_t now_ns);

/* The connection feeding s closed. Returns true if s was up and is now lost. */
bool health_eof(struct health *h, enum sensor s);

/* Marks sensors silent for longer than their loss timeout as lost. Returns a
 * mask of (1u << sensor) for each one lost by this call. */
unsigned health_tick(struct health *h, int64_t now_ns);

enum health_state health_state(const struct health *h);
const char *sensor_name(enum sensor s);

#endif
