/* Store log records (DESIGN.md §4). An 11-byte little-endian header, then the
 * payload exactly as received:
 *
 *   len u16 (payload bytes) | type u8 | seq u32 | crc32 u32 over len+type+seq+payload
 *
 * For a received message the type is its msg type byte; REC_GAP is written by
 * store itself at every start and never travels on the wire.
 */
#ifndef DLBY_RECORD_H
#define DLBY_RECORD_H

#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>

#define REC_HDR_LEN     11
#define REC_MAX_PAYLOAD 255
#define REC_GAP         0x80	/* payload: u32 truncated_bytes, u32 pad */
#define REC_GAP_LEN     8

struct rec {
	uint16_t len;
	uint8_t  type;
	uint32_t seq;
	uint32_t crc;
};

static inline void put_le16(uint8_t *p, uint16_t v)
{
	p[0] = (uint8_t)v;
	p[1] = (uint8_t)(v >> 8);
}

static inline void put_le32(uint8_t *p, uint32_t v)
{
	p[0] = (uint8_t)v;
	p[1] = (uint8_t)(v >> 8);
	p[2] = (uint8_t)(v >> 16);
	p[3] = (uint8_t)(v >> 24);
}

static inline uint32_t get_le32(const uint8_t *p)
{
	return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}

/* CRC-32 (IEEE 802.3, as zlib.crc32). Chain calls by passing the previous result; start at 0. */
uint32_t crc32_ieee(uint32_t crc, const void *buf, size_t n);

/* Encode into out (>= REC_HDR_LEN + len bytes; 1 <= len <= REC_MAX_PAYLOAD). Returns the total length. */
size_t rec_encode(uint8_t *out, uint8_t type, uint32_t seq, const void *payload, size_t len);

/* Check the record at b. Returns its total length, 0 if more bytes are needed,
 * or -1 if it is invalid (bad len or CRC). */
ssize_t rec_check(const uint8_t *b, size_t avail, struct rec *r);

/* Called for each valid record; a negative return stops the scan with -1. */
typedef int (*rec_cb)(void *ctx, const struct rec *r, const uint8_t *payload);

/* Walk fd's records from offset 0 up to the first invalid or incomplete one.
 * *good_end is the offset just past the last valid record, *file_end the file
 * size; they differ when the tail is torn. Returns 0, or -1 with errno set. */
int rec_scan(int fd, rec_cb cb, void *ctx, off_t *good_end, off_t *file_end);

#endif
