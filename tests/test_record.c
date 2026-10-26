#include "check.h"
#include "record.h"

static void test_crc_vector(void)
{
	CHECK(crc32_ieee(0, "123456789", 9) == 0xCBF43926u);
	/* chained calls equal one call */
	CHECK(crc32_ieee(crc32_ieee(0, "1234", 4), "56789", 5) == 0xCBF43926u);
}

static void test_round_trip(void)
{
	uint8_t payload[24], buf[REC_HDR_LEN + 24];
	struct rec r;
	size_t i;

	for (i = 0; i < sizeof(payload); i++)
		payload[i] = (uint8_t)i;
	CHECK(rec_encode(buf, 2, 0x01020304u, payload, sizeof(payload)) == sizeof(buf));
	CHECK(buf[0] == 24 && buf[1] == 0 && buf[2] == 2 && buf[3] == 0x04 && buf[6] == 0x01);
	CHECK(rec_check(buf, sizeof(buf), &r) == (ssize_t)sizeof(buf));
	CHECK(r.len == 24 && r.type == 2 && r.seq == 0x01020304u);
	CHECK(memcmp(buf + REC_HDR_LEN, payload, sizeof(payload)) == 0);
}

static void test_rejects_damage(void)
{
	uint8_t payload[16], buf[REC_HDR_LEN + 16], bad[sizeof(buf)];
	struct rec r;

	memset(payload, 0xab, sizeof(payload));
	rec_encode(buf, 3, 7, payload, sizeof(payload));

	memcpy(bad, buf, sizeof(buf));
	bad[REC_HDR_LEN + 5] ^= 0x01;		/* payload bit */
	CHECK(rec_check(bad, sizeof(bad), &r) == -1);

	memcpy(bad, buf, sizeof(buf));
	bad[3] ^= 0x80;				/* seq bit */
	CHECK(rec_check(bad, sizeof(bad), &r) == -1);

	memcpy(bad, buf, sizeof(buf));
	bad[0] = 0;				/* len = 0 */
	bad[1] = 0;
	CHECK(rec_check(bad, sizeof(bad), &r) == -1);

	memcpy(bad, buf, sizeof(buf));
	bad[1] = 1;				/* len = 272 > max */
	CHECK(rec_check(bad, sizeof(bad), &r) == -1);

	/* cut short: needs more bytes, not invalid */
	CHECK(rec_check(buf, sizeof(buf) - 1, &r) == 0);
	CHECK(rec_check(buf, REC_HDR_LEN - 1, &r) == 0);
}

int main(void)
{
	RUN(test_crc_vector);
	RUN(test_round_trip);
	RUN(test_rejects_damage);
	return 0;
}
