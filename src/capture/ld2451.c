#include "ld2451.h"

#include <errno.h>
#include <string.h>

int ld2451_decode(const uint8_t *payload, size_t len, struct radar_msg *m)
{
	size_t i, n;

	m->ntargets = 0;
	memset(m->t, 0, sizeof(m->t));
	if (len < LD2451_HDR_LEN)
		goto bad;
	n = payload[0];
	if (n > RADAR_MAX_TARGETS || len != LD2451_HDR_LEN + n * LD2451_TARGET_LEN)
		goto bad;

	for (i = 0; i < n; i++) {
		const uint8_t *b = payload + LD2451_HDR_LEN + i * LD2451_TARGET_LEN;
		struct radar_target *t = &m->t[i];
		/* km/h -> cm/s, rounded: 255 km/h is 7083 cm/s, well inside int16. */
		int16_t cms = (int16_t)((b[3] * 1000 + 18) / 36);

		t->angle_deg = (int8_t)(b[0] - 0x80);
		t->range_cm = (uint16_t)(b[1] * 100);
		t->dir = b[2];
		t->speed_cms = b[2] == LD2451_DIR_APPROACH ? (int16_t)-cms : cms;
		t->snr = b[4];
	}
	m->ntargets = (uint8_t)n;
	return (int)n;

bad:
	errno = EINVAL;
	return -1;
}
