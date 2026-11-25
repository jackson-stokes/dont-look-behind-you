#include "check.h"
#include "ld2451.h"

/* Payloads (frame bytes 6..12) copied from docs/evidence/radar-targets-pi.txt. */
static const uint8_t WALK_9M[]  = { 0x01, 0x01, 0x82, 0x09, 0x01, 0x04, 0xDD };
static const uint8_t JOG_14K[]  = { 0x01, 0x01, 0x7A, 0x06, 0x01, 0x0E, 0x8B };
static const uint8_t LEFT_36[]  = { 0x01, 0x01, 0x5C, 0x08, 0x01, 0x04, 0xB2 };
static const uint8_t RIGHT_53[] = { 0x01, 0x01, 0xB5, 0x02, 0x01, 0x03, 0x83 };

static void test_walk(void)
{
	struct radar_msg m;

	memset(&m, 0, sizeof(m));
	CHECK(ld2451_decode(WALK_9M, sizeof(WALK_9M), &m) == 1);
	CHECK(m.ntargets == 1);
	CHECK(m.t[0].angle_deg == 2);
	CHECK(m.t[0].range_cm == 900);
	CHECK(m.t[0].dir == LD2451_DIR_APPROACH);
	CHECK(m.t[0].speed_cms == -111);	/* 4 km/h = 111.1 cm/s, approaching */
	CHECK(m.t[0].snr == 0xDD);
}

static void test_jog(void)
{
	struct radar_msg m;

	CHECK(ld2451_decode(JOG_14K, sizeof(JOG_14K), &m) == 1);
	CHECK(m.t[0].range_cm == 600);
	CHECK(m.t[0].speed_cms == -389);	/* 14 km/h = 388.9 cm/s */
	CHECK(m.t[0].angle_deg == -6);
}

/* Both ends of the angles seen in the capture. */
static void test_angles(void)
{
	struct radar_msg m;

	CHECK(ld2451_decode(LEFT_36, sizeof(LEFT_36), &m) == 1);
	CHECK(m.t[0].angle_deg == -36);
	CHECK(ld2451_decode(RIGHT_53, sizeof(RIGHT_53), &m) == 1);
	CHECK(m.t[0].angle_deg == 53);
	CHECK(m.t[0].range_cm == 200);
}

/* Never seen on the radar: direction 0 is taken as receding (positive speed). */
static void test_receding(void)
{
	static const uint8_t p[] = { 0x01, 0x01, 0x80, 0x0A, 0x00, 0x24, 0x40 };
	struct radar_msg m;

	CHECK(ld2451_decode(p, sizeof(p), &m) == 1);
	CHECK(m.t[0].dir == 0);
	CHECK(m.t[0].speed_cms == 1000);	/* 36 km/h */
	CHECK(m.t[0].range_cm == 1000);
}

/* Synthetic: two targets, assuming the 5-byte stride holds. */
static void test_two_targets(void)
{
	static const uint8_t p[] = { 0x02, 0x01,
				     0x80, 0x14, 0x01, 0x14, 0x90,
				     0x7B, 0x05, 0x01, 0x03, 0xFF };
	struct radar_msg m;

	CHECK(ld2451_decode(p, sizeof(p), &m) == 2);
	CHECK(m.ntargets == 2);
	CHECK(m.t[0].range_cm == 2000 && m.t[0].speed_cms == -556 && m.t[0].angle_deg == 0);
	CHECK(m.t[1].range_cm == 500 && m.t[1].speed_cms == -83 && m.t[1].angle_deg == -5);
	CHECK(m.t[1].snr == 0xFF);
}

/* Extremes stay in range: 255 m, 255 km/h, angle bytes 0x00 and 0xFF. */
static void test_extremes(void)
{
	static const uint8_t p[] = { 0x02, 0x01,
				     0x00, 0xFF, 0x01, 0xFF, 0xFF,
				     0xFF, 0x00, 0x00, 0x00, 0x00 };
	struct radar_msg m;

	CHECK(ld2451_decode(p, sizeof(p), &m) == 2);
	CHECK(m.t[0].angle_deg == -128 && m.t[0].range_cm == 25500 && m.t[0].speed_cms == -7083);
	CHECK(m.t[1].angle_deg == 127 && m.t[1].range_cm == 0 && m.t[1].speed_cms == 0);
}

/* A count of 0 with no target bytes is valid and clears stale targets. */
static void test_zero_targets(void)
{
	static const uint8_t p[] = { 0x00, 0x00 };
	struct radar_msg m;

	CHECK(ld2451_decode(WALK_9M, sizeof(WALK_9M), &m) == 1);
	CHECK(ld2451_decode(p, sizeof(p), &m) == 0);
	CHECK(m.ntargets == 0 && m.t[0].range_cm == 0);
}

/* Length must be exactly 2 + 5 * count; failures leave no targets. */
static void test_bad_lengths(void)
{
	static const uint8_t four[] = { 0x01, 0x02, 0x03, 0x04 };	/* fake-ld2451's old payload */
	struct radar_msg m;

	CHECK(ld2451_decode(WALK_9M, sizeof(WALK_9M), &m) == 1);
	errno = 0;
	CHECK(ld2451_decode(WALK_9M, sizeof(WALK_9M) - 1, &m) == -1 && errno == EINVAL);
	CHECK(m.ntargets == 0 && m.t[0].range_cm == 0);
	CHECK(ld2451_decode(four, sizeof(four), &m) == -1 && errno == EINVAL);
	CHECK(ld2451_decode(WALK_9M, 1, &m) == -1 && errno == EINVAL);
	CHECK(ld2451_decode(WALK_9M, 0, &m) == -1 && errno == EINVAL);
}

/* More targets than radar_msg holds is rejected, never truncated or overrun. */
static void test_too_many(void)
{
	uint8_t p[LD2451_HDR_LEN + (RADAR_MAX_TARGETS + 1) * LD2451_TARGET_LEN];
	struct radar_msg m;

	memset(p, 0x80, sizeof(p));
	p[0] = RADAR_MAX_TARGETS + 1;
	CHECK(ld2451_decode(p, sizeof(p), &m) == -1 && errno == EINVAL);
	CHECK(m.ntargets == 0);
	p[0] = 0xFF;
	CHECK(ld2451_decode(p, sizeof(p), &m) == -1 && errno == EINVAL);

	p[0] = RADAR_MAX_TARGETS;
	CHECK(ld2451_decode(p, sizeof(p) - LD2451_TARGET_LEN, &m) == RADAR_MAX_TARGETS);
}

/* Header fields of the message are left alone. */
static void test_keeps_header(void)
{
	struct radar_msg m;

	memset(&m, 0, sizeof(m));
	m.type = MSG_RADAR;
	m.side = RADAR_LEFT;
	m.seq = 42;
	CHECK(ld2451_decode(WALK_9M, sizeof(WALK_9M), &m) == 1);
	CHECK(m.type == MSG_RADAR && m.side == RADAR_LEFT && m.seq == 42);
}

int main(void)
{
	RUN(test_walk);
	RUN(test_jog);
	RUN(test_angles);
	RUN(test_receding);
	RUN(test_two_targets);
	RUN(test_extremes);
	RUN(test_zero_targets);
	RUN(test_bad_lengths);
	RUN(test_too_many);
	RUN(test_keeps_header);
	return 0;
}
