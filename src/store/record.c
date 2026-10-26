#include "record.h"

#include <errno.h>
#include <stdbool.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define SCAN_BUF 16384	/* far larger than one record */

static uint32_t crc_table[256];
static bool crc_ready;

static void crc_init(void)
{
	uint32_t c;
	int i, k;

	for (i = 0; i < 256; i++) {
		c = (uint32_t)i;
		for (k = 0; k < 8; k++)
			c = (c & 1) ? 0xEDB88320u ^ (c >> 1) : c >> 1;
		crc_table[i] = c;
	}
	crc_ready = true;
}

uint32_t crc32_ieee(uint32_t crc, const void *buf, size_t n)
{
	const uint8_t *p = buf;

	if (!crc_ready)
		crc_init();
	crc = ~crc;
	while (n--)
		crc = crc_table[(crc ^ *p++) & 0xff] ^ (crc >> 8);
	return ~crc;
}

size_t rec_encode(uint8_t *out, uint8_t type, uint32_t seq, const void *payload, size_t len)
{
	uint32_t crc;

	put_le16(out, (uint16_t)len);
	out[2] = type;
	put_le32(out + 3, seq);
	memcpy(out + REC_HDR_LEN, payload, len);
	crc = crc32_ieee(0, out, 7);
	crc = crc32_ieee(crc, out + REC_HDR_LEN, len);
	put_le32(out + 7, crc);
	return REC_HDR_LEN + len;
}

ssize_t rec_check(const uint8_t *b, size_t avail, struct rec *r)
{
	uint32_t crc;

	if (avail < REC_HDR_LEN)
		return 0;
	r->len = (uint16_t)(b[0] | b[1] << 8);
	r->type = b[2];
	r->seq = get_le32(b + 3);
	r->crc = get_le32(b + 7);
	if (r->len == 0 || r->len > REC_MAX_PAYLOAD)
		return -1;
	if (avail < (size_t)REC_HDR_LEN + r->len)
		return 0;
	crc = crc32_ieee(0, b, 7);
	crc = crc32_ieee(crc, b + REC_HDR_LEN, r->len);
	if (crc != r->crc)
		return -1;
	return REC_HDR_LEN + r->len;
}

int rec_scan(int fd, rec_cb cb, void *ctx, off_t *good_end, off_t *file_end)
{
	uint8_t buf[SCAN_BUF];
	size_t have = 0, used;
	off_t pos = 0;		/* file offset of buf[0] */
	struct stat st;
	struct rec r;
	ssize_t n, len;

	if (fstat(fd, &st) < 0 || lseek(fd, 0, SEEK_SET) < 0)
		return -1;
	for (;;) {
		n = read(fd, buf + have, sizeof(buf) - have);
		if (n < 0) {
			if (errno == EINTR)
				continue;
			return -1;
		}
		have += (size_t)n;
		used = 0;
		while ((len = rec_check(buf + used, have - used, &r)) > 0) {
			if (cb && cb(ctx, &r, buf + used + REC_HDR_LEN) < 0)
				return -1;
			used += (size_t)len;
		}
		pos += (off_t)used;
		memmove(buf, buf + used, have - used);
		have -= used;
		if (len < 0 || n == 0)
			break;		/* invalid record, or a partial one at EOF */
	}
	*good_end = pos;
	*file_end = st.st_size;
	return 0;
}
