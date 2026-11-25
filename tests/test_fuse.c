#include "check.h"
#include "fuse.h"

#define MS 1000000LL

#define R   (1u << SENSOR_RADAR)
#define S   (1u << SENSOR_SONAR)
#define L   (1u << SENSOR_RADAR_LEFT)
#define ALL (R | S | L)

/* Evaluate and check what each side shows: right mirror, left mirror. */
#define SIDES(f, up, now, right_st, left_st) do { \
	struct fuse_out o_; \
	fuse_eval((f), (up), (now), &o_); \
	if (o_.right != (right_st) || o_.left != (left_st)) \
		printf("  got right=%s left=%s\n", fuse_state_name(o_.right), \
		       fuse_state_name(o_.left)); \
	CHECK(o_.right == (right_st) && o_.left == (left_st)); \
} while (0)

/* Echo width for a target at cm, at 343 m/s. */
static uint32_t echo_for_cm(uint32_t cm)
{
	return (uint32_t)((uint64_t)cm * 20000000 / 343);
}

static void radar(struct fuse *f, enum sensor s, uint16_t cm, int16_t cms, int64_t now)
{
	struct radar_msg m;

	memset(&m, 0, sizeof(m));
	m.type = MSG_RADAR;
	m.side = s == SENSOR_RADAR_LEFT ? RADAR_LEFT : RADAR_RIGHT;
	if (cm) {
		m.ntargets = 1;
		m.t[0].range_cm = cm;
		m.t[0].speed_cms = cms;
	}
	fuse_radar(f, s, &m, now);
}

static void ping(struct fuse *f, bool hit, int64_t now)
{
	struct sonar_msg m;

	memset(&m, 0, sizeof(m));
	m.type = MSG_SONAR;
	if (hit)
		m.echo_ns = echo_for_cm(150);
	else
		m.flags = SONAR_NO_ECHO;
	fuse_sonar(f, &m, now);
}

static void test_echo_cm(void)
{
	CHECK(fuse_echo_cm(echo_for_cm(150)) >= 149 && fuse_echo_cm(echo_for_cm(150)) <= 150);
	CHECK(fuse_echo_cm(0) == 0);
	CHECK(fuse_echo_cm(UINT32_MAX) > FUSE_ZONE_MAX_CM);	/* no overflow */
}

/* Range limits of a hit: 20..430 cm. */
static void test_zone_range(void)
{
	struct sonar_msg m;
	struct fuse f;

	memset(&m, 0, sizeof(m));
	m.type = MSG_SONAR;
	fuse_init(&f, FUSE_RADAR_PRESENCE, 0);
	m.echo_ns = echo_for_cm(FUSE_ZONE_MAX_CM + 2);
	fuse_sonar(&f, &m, 0);
	fuse_sonar(&f, &m, 0);
	SIDES(&f, ALL, 0, ST_CLEAR, ST_CLEAR);
	m.echo_ns = echo_for_cm(FUSE_ZONE_MIN_CM - 2);
	fuse_sonar(&f, &m, 0);
	fuse_sonar(&f, &m, 0);
	SIDES(&f, ALL, 0, ST_CLEAR, ST_CLEAR);
	m.echo_ns = echo_for_cm(FUSE_ZONE_MAX_CM);
	fuse_sonar(&f, &m, 0);
	fuse_sonar(&f, &m, 0);
	SIDES(&f, ALL, 0, ST_IN_ZONE, ST_CLEAR);
	m.flags = SONAR_STUCK_HIGH;		/* a stuck line is never a hit */
	fuse_sonar(&f, &m, 0);
	fuse_sonar(&f, &m, 0);
	SIDES(&f, ALL, 0, ST_CLEAR, ST_CLEAR);
}

/* Each side comes up with its own sensors. */
static void test_blind_then_clear(void)
{
	struct fuse f;

	fuse_init(&f, FUSE_RADAR_TRACKS, FUSE_HOLD_NS);
	CHECK(f.side[FUSE_RIGHT].state == ST_BLIND && f.side[FUSE_LEFT].state == ST_BLIND);
	SIDES(&f, 0, 0, ST_BLIND, ST_BLIND);
	SIDES(&f, R, 0, ST_DEG_CLEAR, ST_BLIND);
	SIDES(&f, R | S, 0, ST_CLEAR, ST_BLIND);	/* right side whole, left unwatched */
	SIDES(&f, ALL, 0, ST_CLEAR, ST_CLEAR);
	SIDES(&f, L, 0, ST_BLIND, ST_CLEAR);
}

/* A cyclist pass on the right: track -> APPROACHING, 2 of 3 hits -> IN_ZONE. */
static void test_pass(void)
{
	struct fuse f;

	fuse_init(&f, FUSE_RADAR_TRACKS, FUSE_HOLD_NS);
	radar(&f, SENSOR_RADAR, 1500, -400, 0);
	SIDES(&f, ALL, 0, ST_APPROACHING, ST_CLEAR);
	ping(&f, true, 60 * MS);
	SIDES(&f, ALL, 60 * MS, ST_APPROACHING, ST_CLEAR);	/* 1 of 3 */
	ping(&f, true, 120 * MS);
	SIDES(&f, ALL, 120 * MS, ST_IN_ZONE, ST_CLEAR);
	CHECK(f.zone_unconfirmed == 0);
}

static void test_one_dropout_keeps_zone(void)
{
	struct fuse f;

	fuse_init(&f, FUSE_RADAR_TRACKS, 0);	/* no hold: the 2-of-3 rule alone */
	radar(&f, SENSOR_RADAR, 300, -400, 0);
	ping(&f, true, 0);
	ping(&f, true, 60 * MS);
	SIDES(&f, ALL, 60 * MS, ST_IN_ZONE, ST_CLEAR);
	ping(&f, false, 120 * MS);			/* hit hit miss */
	SIDES(&f, ALL, 120 * MS, ST_IN_ZONE, ST_CLEAR);
	ping(&f, false, 180 * MS);			/* hit miss miss */
	SIDES(&f, ALL, 180 * MS, ST_APPROACHING, ST_CLEAR);
}

/* Something in the zone that the radar never tracked: a pole, a parked car. */
static void test_unconfirmed_stays_clear(void)
{
	struct fuse f;
	int i;

	fuse_init(&f, FUSE_RADAR_TRACKS, FUSE_HOLD_NS);
	for (i = 0; i < 5; i++) {
		ping(&f, true, i * 60 * MS);
		SIDES(&f, ALL, i * 60 * MS, ST_CLEAR, ST_CLEAR);
	}
	CHECK(f.zone_unconfirmed == 1);		/* counted once per episode */
	ping(&f, false, 300 * MS);
	ping(&f, false, 360 * MS);
	SIDES(&f, ALL, 360 * MS, ST_CLEAR, ST_CLEAR);
	ping(&f, true, 420 * MS);
	ping(&f, true, 480 * MS);
	SIDES(&f, ALL, 480 * MS, ST_CLEAR, ST_CLEAR);
	CHECK(f.zone_unconfirmed == 2);
}

static void test_receding_and_far_ignored(void)
{
	struct fuse f;

	fuse_init(&f, FUSE_RADAR_TRACKS, FUSE_HOLD_NS);
	radar(&f, SENSOR_RADAR, 1000, 300, 0);			/* moving away */
	SIDES(&f, ALL, 0, ST_CLEAR, ST_CLEAR);
	radar(&f, SENSOR_RADAR, 1000, FUSE_APPROACH_CMS + 1, 0);	/* too slow */
	SIDES(&f, ALL, 0, ST_CLEAR, ST_CLEAR);
	radar(&f, SENSOR_RADAR, FUSE_WATCH_CM + 1, -400, 0);	/* too far */
	SIDES(&f, ALL, 0, ST_CLEAR, ST_CLEAR);
	radar(&f, SENSOR_RADAR, FUSE_WATCH_CM, FUSE_APPROACH_CMS, 0);	/* both limits */
	SIDES(&f, ALL, 0, ST_APPROACHING, ST_CLEAR);
}

/* ntargets past RADAR_MAX_TARGETS must not read past t[]. */
static void test_ntargets_clamped(void)
{
	struct radar_msg m;
	struct fuse f;

	memset(&m, 0, sizeof(m));
	m.type = MSG_RADAR;
	m.ntargets = 255;
	m.t[RADAR_MAX_TARGETS - 1].range_cm = 500;
	m.t[RADAR_MAX_TARGETS - 1].speed_cms = -400;
	fuse_init(&f, FUSE_RADAR_TRACKS, 0);
	fuse_radar(&f, SENSOR_RADAR, &m, 0);
	SIDES(&f, ALL, 0, ST_APPROACHING, ST_CLEAR);
	CHECK(f.track_cm[FUSE_RIGHT] == 500);
}

/* The track lives FUSE_TRACK_TTL_NS; a zone hit confirms up to FUSE_CONFIRM_NS. */
static void test_track_ttl_and_confirm_window(void)
{
	struct fuse f;
	int64_t t;

	fuse_init(&f, FUSE_RADAR_TRACKS, 0);
	radar(&f, SENSOR_RADAR, 800, -400, 0);
	SIDES(&f, ALL, FUSE_TRACK_TTL_NS, ST_APPROACHING, ST_CLEAR);
	SIDES(&f, ALL, FUSE_TRACK_TTL_NS + 1, ST_CLEAR, ST_CLEAR);
	/* The cyclist left the radar's view and reaches the zone 2.5 s later. */
	t = 2500 * MS;
	ping(&f, true, t);
	ping(&f, true, t + 60 * MS);
	SIDES(&f, ALL, t + 60 * MS, ST_IN_ZONE, ST_CLEAR);
	/* Past the confirm window the same hits are unconfirmed. */
	fuse_init(&f, FUSE_RADAR_TRACKS, 0);
	radar(&f, SENSOR_RADAR, 800, -400, 0);
	ping(&f, true, FUSE_CONFIRM_NS);
	ping(&f, true, FUSE_CONFIRM_NS + 1);
	SIDES(&f, ALL, FUSE_CONFIRM_NS + 1, ST_CLEAR, ST_CLEAR);
	CHECK(f.zone_unconfirmed == 1);
}

/* The hold runs from the first evaluation that sees the drop (the hub
 * evaluates every loop, <= 50 ms apart). */
static void test_hold(void)
{
	struct fuse f;

	fuse_init(&f, FUSE_RADAR_TRACKS, FUSE_HOLD_NS);
	radar(&f, SENSOR_RADAR, 300, -400, 0);
	ping(&f, true, 0);
	ping(&f, true, 60 * MS);
	SIDES(&f, ALL, 60 * MS, ST_IN_ZONE, ST_CLEAR);
	ping(&f, false, 120 * MS);
	ping(&f, false, 180 * MS);			/* zone gone at 180 ms */
	SIDES(&f, ALL, 180 * MS, ST_IN_ZONE, ST_CLEAR);
	SIDES(&f, ALL, 180 * MS + FUSE_HOLD_NS - 1, ST_IN_ZONE, ST_CLEAR);
	SIDES(&f, ALL, 180 * MS + FUSE_HOLD_NS, ST_APPROACHING, ST_CLEAR);
	/* Support returning inside the hold cancels it. */
	fuse_init(&f, FUSE_RADAR_TRACKS, FUSE_HOLD_NS);
	radar(&f, SENSOR_RADAR, 800, -400, 0);
	SIDES(&f, ALL, 0, ST_APPROACHING, ST_CLEAR);
	SIDES(&f, ALL, FUSE_TRACK_TTL_NS + 1, ST_APPROACHING, ST_CLEAR);	/* holding */
	CHECK(f.side[FUSE_RIGHT].dropping);
	radar(&f, SENSOR_RADAR, 700, -400, FUSE_TRACK_TTL_NS + 100 * MS);
	SIDES(&f, ALL, FUSE_TRACK_TTL_NS + 100 * MS, ST_APPROACHING, ST_CLEAR);
	CHECK(!f.side[FUSE_RIGHT].dropping);
}

static void test_right_radar_lost(void)
{
	struct fuse f;

	fuse_init(&f, FUSE_RADAR_TRACKS, FUSE_HOLD_NS);
	radar(&f, SENSOR_RADAR, 300, -400, 0);
	ping(&f, true, 0);
	ping(&f, true, 60 * MS);
	SIDES(&f, ALL, 60 * MS, ST_IN_ZONE, ST_CLEAR);
	/* Health changes apply at once, never held. */
	SIDES(&f, S | L, 60 * MS, ST_DEG_IN_ZONE, ST_CLEAR);	/* ultrasonic alone warns */
	ping(&f, false, 120 * MS);
	ping(&f, false, 180 * MS);
	SIDES(&f, S | L, 180 * MS, ST_DEG_IN_ZONE, ST_CLEAR);	/* hold starts */
	SIDES(&f, S | L, 180 * MS + FUSE_HOLD_NS, ST_DEG_CLEAR, ST_CLEAR);
	/* A left track lights the left mirror only. */
	radar(&f, SENSOR_RADAR_LEFT, 900, -400, 200 * MS);
	SIDES(&f, S | L, 200 * MS + FUSE_HOLD_NS, ST_DEG_CLEAR, ST_APPROACHING);
	/* The right radar's old track must not count when it comes back. */
	fuse_init(&f, FUSE_RADAR_TRACKS, 0);
	radar(&f, SENSOR_RADAR, 300, -400, 0);
	SIDES(&f, S | L, 0, ST_DEG_CLEAR, ST_CLEAR);
	SIDES(&f, ALL, 10 * MS, ST_CLEAR, ST_CLEAR);
}

static void test_sonar_lost(void)
{
	struct fuse f;

	fuse_init(&f, FUSE_RADAR_TRACKS, FUSE_HOLD_NS);
	radar(&f, SENSOR_RADAR, 300, -400, 0);
	ping(&f, true, 0);
	ping(&f, true, 60 * MS);
	SIDES(&f, ALL, 60 * MS, ST_IN_ZONE, ST_CLEAR);
	SIDES(&f, R | L, 60 * MS, ST_DEG_APPROACHING, ST_CLEAR);	/* radar alone */
	SIDES(&f, R | L, FUSE_TRACK_TTL_NS + 1, ST_DEG_APPROACHING, ST_CLEAR);	/* hold starts */
	SIDES(&f, R | L, FUSE_TRACK_TTL_NS + 1 + FUSE_HOLD_NS, ST_DEG_CLEAR, ST_CLEAR);
	/* Old pings must not count when the sonar comes back: one new hit isn't 2 of 3. */
	radar(&f, SENSOR_RADAR, 300, -400, 3000 * MS);
	ping(&f, true, 3000 * MS);
	SIDES(&f, ALL, 3000 * MS, ST_APPROACHING, ST_CLEAR);
}

static void test_all_lost_blind(void)
{
	struct fuse f;

	fuse_init(&f, FUSE_RADAR_TRACKS, FUSE_HOLD_NS);
	radar(&f, SENSOR_RADAR, 300, -400, 0);
	SIDES(&f, ALL, 0, ST_APPROACHING, ST_CLEAR);
	SIDES(&f, 0, 10 * MS, ST_BLIND, ST_BLIND);		/* not held */
	SIDES(&f, L, 20 * MS, ST_BLIND, ST_CLEAR);		/* left radar only */
}

/* No left ultrasonic: a left track raises APPROACHING on the left mirror, never
 * IN_ZONE, and never touches the right side. */
static void test_left_radar(void)
{
	struct fuse f;

	fuse_init(&f, FUSE_RADAR_TRACKS, 0);
	radar(&f, SENSOR_RADAR_LEFT, 900, -400, 0);
	SIDES(&f, ALL, 0, ST_CLEAR, ST_APPROACHING);
	/* Right-side zone hits: the left track doesn't confirm them. */
	ping(&f, true, 60 * MS);
	ping(&f, true, 120 * MS);
	SIDES(&f, ALL, 120 * MS, ST_CLEAR, ST_APPROACHING);
	CHECK(f.zone_unconfirmed == 1);
	SIDES(&f, ALL, FUSE_TRACK_TTL_NS + 1, ST_CLEAR, ST_CLEAR);
	/* A target on an unknown sensor is ignored. */
	fuse_init(&f, FUSE_RADAR_TRACKS, 0);
	radar(&f, SENSOR_SONAR, 300, -400, 0);
	SIDES(&f, ALL, 0, ST_CLEAR, ST_CLEAR);
}

/* Losing the left radar leaves the right side's state and hold alone. */
static void test_left_loss_leaves_right(void)
{
	struct fuse f;

	fuse_init(&f, FUSE_RADAR_TRACKS, FUSE_HOLD_NS);
	radar(&f, SENSOR_RADAR, 300, -400, 0);
	ping(&f, true, 0);
	ping(&f, true, 60 * MS);
	SIDES(&f, ALL, 60 * MS, ST_IN_ZONE, ST_CLEAR);
	SIDES(&f, R | S, 70 * MS, ST_IN_ZONE, ST_BLIND);
	CHECK(!f.side[FUSE_RIGHT].dropping);
	SIDES(&f, ALL, 80 * MS, ST_IN_ZONE, ST_CLEAR);
	/* With nothing on the right, CLEAR stays CLEAR (not DEG_CLEAR). */
	fuse_init(&f, FUSE_RADAR_TRACKS, 0);
	SIDES(&f, R | S, 0, ST_CLEAR, ST_BLIND);
}

/* Each side's hold runs on its own. */
static void test_holds_independent(void)
{
	struct fuse f;

	fuse_init(&f, FUSE_RADAR_TRACKS, FUSE_HOLD_NS);
	radar(&f, SENSOR_RADAR, 800, -400, 0);
	radar(&f, SENSOR_RADAR_LEFT, 800, -400, 0);
	SIDES(&f, ALL, 0, ST_APPROACHING, ST_APPROACHING);
	radar(&f, SENSOR_RADAR, 700, -400, 1000 * MS);	/* only the right is refreshed */
	SIDES(&f, ALL, FUSE_TRACK_TTL_NS + 1, ST_APPROACHING, ST_APPROACHING);
	CHECK(f.side[FUSE_LEFT].dropping && !f.side[FUSE_RIGHT].dropping);
	SIDES(&f, ALL, FUSE_TRACK_TTL_NS + 1 + FUSE_HOLD_NS, ST_APPROACHING, ST_CLEAR);
}

/* Fallback when radar targets aren't decoded: the zone alone warns. */
static void test_presence_mode(void)
{
	struct fuse f;

	fuse_init(&f, FUSE_RADAR_PRESENCE, 0);
	radar(&f, SENSOR_RADAR, 300, -400, 0);		/* targets ignored */
	radar(&f, SENSOR_RADAR_LEFT, 300, -400, 0);
	SIDES(&f, ALL, 0, ST_CLEAR, ST_CLEAR);
	ping(&f, true, 60 * MS);
	SIDES(&f, ALL, 60 * MS, ST_CLEAR, ST_CLEAR);
	ping(&f, true, 120 * MS);
	SIDES(&f, ALL, 120 * MS, ST_IN_ZONE, ST_CLEAR);
	CHECK(f.zone_unconfirmed == 0);
	/* Radar health still drives DEG and BLIND. */
	SIDES(&f, S | L, 120 * MS, ST_DEG_IN_ZONE, ST_CLEAR);
	ping(&f, false, 180 * MS);
	ping(&f, false, 240 * MS);
	SIDES(&f, S | L, 240 * MS, ST_DEG_CLEAR, ST_CLEAR);
	SIDES(&f, R | L, 240 * MS, ST_DEG_CLEAR, ST_CLEAR);	/* sonar lost: no track */
	SIDES(&f, R | S, 240 * MS, ST_CLEAR, ST_BLIND);
	SIDES(&f, 0, 240 * MS, ST_BLIND, ST_BLIND);
}

static void test_parse_and_names(void)
{
	enum fuse_radar_mode m = FUSE_RADAR_PRESENCE;

	CHECK(fuse_parse_mode(NULL, &m) == 0 && m == FUSE_RADAR_TRACKS);
	m = FUSE_RADAR_PRESENCE;
	CHECK(fuse_parse_mode("", &m) == 0 && m == FUSE_RADAR_TRACKS);
	CHECK(fuse_parse_mode("tracks", &m) == 0 && m == FUSE_RADAR_TRACKS);
	CHECK(fuse_parse_mode("presence", &m) == 0 && m == FUSE_RADAR_PRESENCE);
	errno = 0;
	CHECK(fuse_parse_mode("Presence", &m) == -1 && errno == EINVAL);
	CHECK(fuse_parse_mode("track", &m) == -1);
	CHECK(strcmp(fuse_mode_name(FUSE_RADAR_PRESENCE), "presence") == 0);
	CHECK(strcmp(fuse_mode_name(FUSE_RADAR_TRACKS), "tracks") == 0);
	CHECK(strcmp(fuse_state_name(ST_DEG_IN_ZONE), "DEG_IN_ZONE") == 0);
	CHECK(strcmp(fuse_state_name(ST_BLIND), "BLIND") == 0);
	CHECK(strcmp(fuse_state_name((enum fuse_state)99), "UNKNOWN") == 0);
}

int main(void)
{
	RUN(test_echo_cm);
	RUN(test_zone_range);
	RUN(test_blind_then_clear);
	RUN(test_pass);
	RUN(test_one_dropout_keeps_zone);
	RUN(test_unconfirmed_stays_clear);
	RUN(test_receding_and_far_ignored);
	RUN(test_ntargets_clamped);
	RUN(test_track_ttl_and_confirm_window);
	RUN(test_hold);
	RUN(test_right_radar_lost);
	RUN(test_sonar_lost);
	RUN(test_all_lost_blind);
	RUN(test_left_radar);
	RUN(test_left_loss_leaves_right);
	RUN(test_holds_independent);
	RUN(test_presence_mode);
	RUN(test_parse_and_names);
	return 0;
}
