#include "check.h"
#include "health.h"

#define MS 1000000LL

static void test_starts_blind(void)
{
	struct health h;

	health_init(&h);
	CHECK(health_state(&h) == H_BLIND);
	CHECK(health_tick(&h, 10000 * MS) == 0);
}

/* OK needs every sensor: both radars and the sonar. */
static void test_all_up_is_ok(void)
{
	struct health h;

	health_init(&h);
	CHECK(health_rx(&h, SENSOR_RADAR, 0));
	CHECK(health_state(&h) == H_DEG);
	CHECK(!health_rx(&h, SENSOR_RADAR, 100 * MS));	/* already up */
	CHECK(health_rx(&h, SENSOR_SONAR, 100 * MS));
	CHECK(health_state(&h) == H_DEG);		/* left radar still missing */
	CHECK(health_rx(&h, SENSOR_RADAR_LEFT, 100 * MS));
	CHECK(health_state(&h) == H_OK);
}

/* The left radar is its own sensor: the right one can't hide its loss. */
static void test_left_radar_loss(void)
{
	struct health h;

	health_init(&h);
	health_rx(&h, SENSOR_RADAR, 0);
	health_rx(&h, SENSOR_SONAR, 0);
	health_rx(&h, SENSOR_RADAR_LEFT, 0);
	health_rx(&h, SENSOR_RADAR, RADAR_LOSS_NS);	/* right radar and sonar stay fresh */
	health_rx(&h, SENSOR_SONAR, RADAR_LOSS_NS);
	CHECK(health_tick(&h, RADAR_LOSS_NS) == 0);
	CHECK(health_tick(&h, RADAR_LOSS_NS + 1) == (1u << SENSOR_RADAR_LEFT));
	CHECK(health_state(&h) == H_DEG && h.up[SENSOR_RADAR] && !h.up[SENSOR_RADAR_LEFT]);
	CHECK(health_rx(&h, SENSOR_RADAR_LEFT, RADAR_LOSS_NS + 2));
	CHECK(health_state(&h) == H_OK);
	CHECK(health_eof(&h, SENSOR_RADAR_LEFT));
	CHECK(health_state(&h) == H_DEG && h.up[SENSOR_RADAR]);
}

static void test_names(void)
{
	CHECK(strcmp(sensor_name(SENSOR_RADAR), "radar") == 0);
	CHECK(strcmp(sensor_name(SENSOR_SONAR), "sonar") == 0);
	CHECK(strcmp(sensor_name(SENSOR_RADAR_LEFT), "radar_left") == 0);
	CHECK(strcmp(sensor_name((enum sensor)SENSOR_N), "unknown") == 0);
}

static void test_sonar_loss_boundary(void)
{
	struct health h;

	CHECK(SONAR_LOSS_NS == 480 * MS);
	health_init(&h);
	health_rx(&h, SENSOR_RADAR, 0);
	health_rx(&h, SENSOR_SONAR, 0);
	health_rx(&h, SENSOR_RADAR_LEFT, 0);
	health_rx(&h, SENSOR_RADAR, SONAR_LOSS_NS);	/* keep the radars fresh */
	health_rx(&h, SENSOR_RADAR_LEFT, SONAR_LOSS_NS);
	CHECK(health_tick(&h, SONAR_LOSS_NS) == 0);
	CHECK(health_state(&h) == H_OK);
	CHECK(health_tick(&h, SONAR_LOSS_NS + 1) == (1u << SENSOR_SONAR));
	CHECK(health_state(&h) == H_DEG && h.up[SENSOR_RADAR] && !h.up[SENSOR_SONAR]);
	CHECK(health_tick(&h, SONAR_LOSS_NS + 2) == 0);	/* reported once */
}

static void test_radar_loss_boundary(void)
{
	struct health h;

	CHECK(RADAR_LOSS_NS == 3000 * MS);
	health_init(&h);
	health_rx(&h, SENSOR_RADAR, 0);
	CHECK(health_tick(&h, RADAR_LOSS_NS) == 0);
	CHECK(health_tick(&h, RADAR_LOSS_NS + 1) == (1u << SENSOR_RADAR));
	CHECK(health_state(&h) == H_BLIND);
}

/* Bench: one empty frame every ~1394 ms with no targets (docs/evidence/sensor-check.txt). */
static void test_empty_frame_rate_keeps_radar_up(void)
{
	const int64_t frame = 1394 * MS;
	struct health h;
	int64_t t, t_frame, prev = 0;
	int k;

	health_init(&h);
	for (k = 0; k <= 21; k++) {
		if (k == 20)
			continue;		/* one missed frame: a 2788 ms gap */
		t_frame = k * frame;
		for (t = prev + 50 * MS; t < t_frame; t += 50 * MS)
			CHECK(health_tick(&h, t) == 0);
		CHECK(health_rx(&h, SENSOR_RADAR, t_frame) == (k == 0));
		CHECK(health_tick(&h, t_frame) == 0);
		prev = t_frame;
	}
	CHECK(h.up[SENSOR_RADAR]);

	/* frames stop: lost exactly one timeout after the last */
	CHECK(health_tick(&h, prev + RADAR_LOSS_NS) == 0);
	CHECK(health_tick(&h, prev + RADAR_LOSS_NS + 1) == (1u << SENSOR_RADAR));
}

static void test_eof_loses_at_once(void)
{
	struct health h;

	health_init(&h);
	health_rx(&h, SENSOR_RADAR, 0);
	health_rx(&h, SENSOR_SONAR, 0);
	CHECK(health_eof(&h, SENSOR_RADAR));
	CHECK(health_state(&h) == H_DEG);
	CHECK(!health_eof(&h, SENSOR_RADAR));	/* already lost */
	CHECK(health_eof(&h, SENSOR_SONAR));
	CHECK(health_state(&h) == H_BLIND);
}

static void test_both_stale_together(void)
{
	struct health h;

	health_init(&h);
	health_rx(&h, SENSOR_RADAR, 0);
	health_rx(&h, SENSOR_SONAR, 0);
	CHECK(health_tick(&h, RADAR_LOSS_NS + 1) == ((1u << SENSOR_RADAR) | (1u << SENSOR_SONAR)));
	CHECK(health_state(&h) == H_BLIND);
}

static void test_recovery(void)
{
	struct health h;

	health_init(&h);
	health_rx(&h, SENSOR_RADAR, 0);
	health_rx(&h, SENSOR_SONAR, 0);
	health_eof(&h, SENSOR_RADAR);
	health_eof(&h, SENSOR_SONAR);
	CHECK(health_state(&h) == H_BLIND);
	CHECK(health_rx(&h, SENSOR_SONAR, 5000 * MS));
	CHECK(health_state(&h) == H_DEG);
	CHECK(health_rx(&h, SENSOR_RADAR, 5000 * MS));
	CHECK(health_state(&h) == H_DEG);		/* left radar never came up */
	CHECK(health_rx(&h, SENSOR_RADAR_LEFT, 5000 * MS));
	CHECK(health_state(&h) == H_OK);
}

int main(void)
{
	RUN(test_starts_blind);
	RUN(test_all_up_is_ok);
	RUN(test_left_radar_loss);
	RUN(test_names);
	RUN(test_sonar_loss_boundary);
	RUN(test_radar_loss_boundary);
	RUN(test_empty_frame_rate_keeps_radar_up);
	RUN(test_eof_loses_at_once);
	RUN(test_both_stale_together);
	RUN(test_recovery);
	return 0;
}
