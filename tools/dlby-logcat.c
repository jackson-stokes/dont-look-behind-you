/* Verify and summarise a store log directory. Never deployed; the seed of the
 * replay harness, which will read the same records.
 *
 *   dlby-logcat [dir]      (default $DLBY_LOG_DIR, else /var/lib/dlby/log)
 *
 * Prints one key=value line. bad= counts bytes after the last valid record of
 * any segment; contiguous=1 means seq rises by exactly 1 across all segments.
 * Exit: 0 if bad=0 and contiguous=1, 1 otherwise, 70 on I/O errors.
 */
#include "msg.h"
#include "record.h"
#include "seglog.h"

#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sysexits.h>
#include <unistd.h>

struct summary {
	uint64_t records, radar, sonar, alert, gap_msg, gap_dropped, store_gap, other;
	uint64_t bad;
	uint32_t first_seq, last_seq;
	bool     any, contiguous;
};

static int count_cb(void *ctx, const struct rec *r, const uint8_t *payload)
{
	struct summary *s = ctx;
	struct gap_msg g;

	if (!s->any) {
		s->first_seq = r->seq;
		s->any = true;
	} else if (r->seq != s->last_seq + 1) {
		s->contiguous = false;
	}
	s->last_seq = r->seq;
	s->records++;
	switch (r->type) {
	case MSG_RADAR: s->radar++; break;
	case MSG_SONAR: s->sonar++; break;
	case MSG_ALERT: s->alert++; break;
	case REC_GAP:   s->store_gap++; break;
	case MSG_GAP:
		s->gap_msg++;
		if (r->len == sizeof(g)) {
			memcpy(&g, payload, sizeof(g));
			s->gap_dropped += g.dropped;
		}
		break;
	default:        s->other++; break;
	}
	return 0;
}

int main(int argc, char **argv)
{
	struct summary s = { .contiguous = true };
	struct seglog_cfg cfg;
	char path[512];
	uint32_t *v;
	off_t good, end;
	size_t n, i;
	int fd;

	seglog_default_cfg(&cfg);
	if (argc > 2) {
		fprintf(stderr, "usage: dlby-logcat [dir]\n");
		return EX_USAGE;
	}
	if (argc == 2)
		cfg.dir = argv[1];
	if (seg_list(cfg.dir, &v, &n) < 0) {
		fprintf(stderr, "dlby-logcat: %s: %s\n", cfg.dir, strerror(errno));
		return EX_SOFTWARE;
	}
	for (i = 0; i < n; i++) {
		snprintf(path, sizeof(path), "%s/", cfg.dir);
		seg_name(path + strlen(path), sizeof(path) - strlen(path), v[i]);
		fd = open(path, O_RDONLY | O_CLOEXEC);
		if (fd < 0 || rec_scan(fd, count_cb, &s, &good, &end) < 0) {
			fprintf(stderr, "dlby-logcat: %s: %s\n", path, strerror(errno));
			if (fd >= 0)
				(void)close(fd);
			free(v);
			return EX_SOFTWARE;
		}
		s.bad += (uint64_t)(end - good);
		if (close(fd) < 0) {
			fprintf(stderr, "dlby-logcat: close %s: %s\n", path, strerror(errno));
			free(v);
			return EX_SOFTWARE;
		}
	}
	free(v);

	printf("segments=%zu records=%" PRIu64 " radar=%" PRIu64 " sonar=%" PRIu64 " alert=%" PRIu64
	       " gap_msg=%" PRIu64 " gap_dropped=%" PRIu64 " store_gap=%" PRIu64 " other=%" PRIu64
	       " bad=%" PRIu64 " first_seq=%" PRIu32 " last_seq=%" PRIu32 " contiguous=%d\n",
	       n, s.records, s.radar, s.sonar, s.alert, s.gap_msg, s.gap_dropped, s.store_gap,
	       s.other, s.bad, s.first_seq, s.last_seq, s.contiguous ? 1 : 0);
	return (s.bad == 0 && s.contiguous) ? 0 : 1;
}
