#include "radar_parse.h"

#include <string.h>

static const uint8_t HDR[4]  = { 0xF4, 0xF3, 0xF2, 0xF1 };
static const uint8_t TAIL[4] = { 0xF8, 0xF7, 0xF6, 0xF5 };

void radar_parser_init(struct radar_parser *p)
{
	memset(p, 0, sizeof(*p));
}

enum radar_feed radar_feed(struct radar_parser *p, uint8_t b)
{
	if (p->n < sizeof(HDR)) {
		if (b == HDR[p->n]) {
			p->buf[p->n++] = b;
		} else if (b == HDR[0]) {	/* restart on a fresh F4 */
			p->buf[0] = b;
			p->n = 1;
		} else {
			p->n = 0;
		}
		return RADAR_MORE;
	}

	p->buf[p->n++] = b;
	if (p->n == 6) {
		p->len = (size_t)p->buf[4] | ((size_t)p->buf[5] << 8);
		if (p->len > RADAR_PAYLOAD_MAX) {
			p->n = 0;
			p->bad_len++;
			return RADAR_BAD_LEN;
		}
		return RADAR_MORE;
	}
	if (p->n < RADAR_FRAME_OVERHEAD + p->len)
		return RADAR_MORE;

	p->n = 0;
	if (memcmp(&p->buf[6 + p->len], TAIL, sizeof(TAIL)) != 0) {
		p->bad_tail++;
		return RADAR_BAD_TAIL;
	}
	p->frames++;
	return RADAR_FRAME;
}
