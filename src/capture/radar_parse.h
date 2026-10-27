/* LD2451 data-frame sync for cap-radar. Pure: bytes in, whole frames out, no
 * I/O, so it is unit-tested directly.
 *
 * Frame: F4 F3 F2 F1, u16 LE payload length, payload, F8 F7 F6 F5. With no
 * targets the payload is empty (docs/evidence/pi-sensor-smoke.txt). The target
 * layout inside the payload is not verified yet, so nothing here decodes it.
 */
#ifndef DLBY_RADAR_PARSE_H
#define DLBY_RADAR_PARSE_H

#include <stddef.h>
#include <stdint.h>

#define RADAR_FRAME_OVERHEAD 10		/* header 4 + length 2 + tail 4 */
#define RADAR_FRAME_MAX      128	/* whole frame; longer lengths are rejected */
#define RADAR_PAYLOAD_MAX    (RADAR_FRAME_MAX - RADAR_FRAME_OVERHEAD)

enum radar_feed {
	RADAR_MORE,		/* nothing complete yet */
	RADAR_FRAME,		/* a whole frame is in buf */
	RADAR_BAD_LEN,		/* length field over RADAR_PAYLOAD_MAX; resyncing */
	RADAR_BAD_TAIL,		/* tail bytes wrong; resyncing */
};

struct radar_parser {
	uint8_t  buf[RADAR_FRAME_MAX];
	size_t   n;		/* bytes of the current frame held in buf */
	size_t   len;		/* payload length, valid once n >= 6 */
	uint64_t frames, bad_len, bad_tail;
};

void radar_parser_init(struct radar_parser *p);

/* Feed one byte. After RADAR_FRAME the frame stays in p->buf, payload at
 * p->buf + 6 with p->len bytes, until the next call. */
enum radar_feed radar_feed(struct radar_parser *p, uint8_t b);

#endif
