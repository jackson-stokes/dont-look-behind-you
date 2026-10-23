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

static void test_both_up_is_ok(void)
{
	struct health h;

	health_init(&h);
	CHECK(health_rx(&h, SENSOR_RADAR, 0));
	CHECK(health_state(&h) == H_DEG);
	CHECK(!health_rx(&h, SENSOR_RADAR, 100 * MS));	/* already up */
	CHECK(health_rx(&h, SENSOR_SONAR, 100 * MS));
	CHECK(health_state(&h) == H_OK);
}

static void test_sonar_loss_boundary(void)
{
	struct health h;

	CHECK(SONAR_LOSS_NS == 480 * MS);
	health_init(&h);
	health_rx(&h, SENSOR_RADAR, 0);
	health_rx(&h, SENSOR_SONAR, 0);
	health_rx(&h, SENSOR_RADAR, SONAR_LOSS_NS);	/* keep radar fresh */
	CHECK(health_tick(&h, SONAR_LOSS_NS) == 0);
	CHECK(health_state(&h) == H_OK);
	CHECK(health_tick(&h, SONAR_LOSS_NS + 1) == (1u << SENSOR_SONAR));
	CHECK(health_state(&h) == H_DEG && h.up[SENSOR_RADAR] && !h.up[SENSOR_SONAR]);
	CHECK(health_tick(&h, SONAR_LOSS_NS + 2) == 0);	/* reported once */
}

static void test_radar_loss_boundary(void)
{
	struct health h;

	CHECK(RADAR_LOSS_NS == 1000 * MS);
	health_init(&h);
	health_rx(&h, SENSOR_RADAR, 0);
	CHECK(health_tick(&h, RADAR_LOSS_NS) == 0);
	CHECK(health_tick(&h, RADAR_LOSS_NS + 1) == (1u << SENSOR_RADAR));
	CHECK(health_state(&h) == H_BLIND);
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
	CHECK(health_state(&h) == H_OK);
}

int main(void)
{
	RUN(test_starts_blind);
	RUN(test_both_up_is_ok);
	RUN(test_sonar_loss_boundary);
	RUN(test_radar_loss_boundary);
	RUN(test_eof_loses_at_once);
	RUN(test_both_stale_together);
	RUN(test_recovery);
	return 0;
}
