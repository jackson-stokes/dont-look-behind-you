#include "check.h"
#include "msg.h"
#include "record.h"
#include "seglog.h"

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#define MS 1000000LL
#define MAX_RECS 4096

static char base[] = "/tmp/dlby-seglog-XXXXXX";

/* Everything readable across a directory's segments. */
struct scan {
	size_t   n;
	uint8_t  type[MAX_RECS];
	uint32_t seq[MAX_RECS];
	uint32_t last_gap_truncated;
	uint64_t bad;
	bool     contiguous;
};

static int scan_cb(void *ctx, const struct rec *r, const uint8_t *payload)
{
	struct scan *s = ctx;

	CHECK(s->n < MAX_RECS);
	if (s->n && r->seq != s->seq[s->n - 1] + 1)
		s->contiguous = false;
	s->type[s->n] = r->type;
	s->seq[s->n] = r->seq;
	s->n++;
	if (r->type == REC_GAP)
		s->last_gap_truncated = get_le32(payload);
	return 0;
}

static void seg_path(char *buf, size_t cap, const char *dir, uint32_t no)
{
	char name[32];

	seg_name(name, sizeof(name), no);
	snprintf(buf, cap, "%s/%s", dir, name);
}

static void scan_dir(const char *dir, struct scan *s)
{
	char path[256];
	uint32_t *v;
	off_t good, end;
	size_t n, i;
	int fd;

	memset(s, 0, sizeof(*s));
	s->contiguous = true;
	CHECK(seg_list(dir, &v, &n) == 0);
	for (i = 0; i < n; i++) {
		seg_path(path, sizeof(path), dir, v[i]);
		fd = open(path, O_RDONLY | O_CLOEXEC);
		CHECK(fd >= 0);
		CHECK(rec_scan(fd, scan_cb, s, &good, &end) == 0);
		s->bad += (uint64_t)(end - good);
		CHECK(close(fd) == 0);
	}
	free(v);
}

static void make_cfg(struct seglog_cfg *cfg, char *dir, size_t cap, const char *test)
{
	snprintf(dir, cap, "%s/%s", base, test);
	seglog_default_cfg(cfg);
	cfg->dir = dir;
}

/* Messages are zeroed first, as msg.h requires. */
static void append_sonar(struct seglog *sl, uint32_t seq, int64_t now)
{
	struct sonar_msg m;

	memset(&m, 0, sizeof(m));
	m.type = MSG_SONAR;
	m.seq = seq;
	m.origin_ns = (uint64_t)now;
	CHECK(seglog_append(sl, MSG_SONAR, &m, sizeof(m), now) == 0);
}

static int append_alert(struct seglog *sl, int64_t now)
{
	struct alert_msg m;

	memset(&m, 0, sizeof(m));
	m.type = MSG_ALERT;
	m.state = ST_CLEAR;
	m.origin_ns = (uint64_t)now;
	return seglog_append(sl, MSG_ALERT, &m, sizeof(m), now);
}

static void test_reopen_continues_seq(void)
{
	struct seglog_cfg cfg;
	struct seglog sl;
	struct scan s;
	char dir[128];
	uint32_t i;

	make_cfg(&cfg, dir, sizeof(dir), "reopen");
	CHECK(seglog_open(&sl, &cfg, 0) == 0);
	for (i = 0; i < 10; i++)
		append_sonar(&sl, i, 0);
	CHECK(seglog_close(&sl) == 0);

	CHECK(seglog_open(&sl, &cfg, 0) == 0);
	CHECK(sl.truncated_bytes == 0 && sl.seg_no == 2 && sl.next_seq == 13);
	CHECK(seglog_close(&sl) == 0);

	scan_dir(dir, &s);
	CHECK(s.n == 12 && s.bad == 0 && s.contiguous);
	CHECK(s.seq[0] == 1 && s.seq[11] == 12);
	CHECK(s.type[0] == REC_GAP && s.type[1] == MSG_SONAR && s.type[11] == REC_GAP);
}

static void test_torn_tail_garbage(void)
{
	struct seglog_cfg cfg;
	struct seglog sl;
	struct scan s;
	char dir[128], path[256];
	uint32_t i;
	int fd;

	make_cfg(&cfg, dir, sizeof(dir), "garbage");
	CHECK(seglog_open(&sl, &cfg, 0) == 0);
	for (i = 0; i < 5; i++)
		append_sonar(&sl, i, 0);	/* seq 2..6 */
	CHECK(seglog_close(&sl) == 0);

	seg_path(path, sizeof(path), dir, 1);
	fd = open(path, O_WRONLY | O_APPEND | O_CLOEXEC);
	CHECK(fd >= 0 && write(fd, "XXXXX", 5) == 5 && close(fd) == 0);

	CHECK(seglog_open(&sl, &cfg, 0) == 0);
	CHECK(sl.truncated_bytes == 5);
	CHECK(sl.next_seq == 8);		/* GAP took 7 */
	CHECK(seglog_close(&sl) == 0);

	scan_dir(dir, &s);
	CHECK(s.bad == 0 && s.contiguous && s.n == 7);
	CHECK(s.seq[5] == 6 && s.type[6] == REC_GAP && s.seq[6] == 7);
	CHECK(s.last_gap_truncated == 5);
}

static void test_torn_tail_half_record(void)
{
	struct seglog_cfg cfg;
	struct seglog sl;
	struct scan s;
	struct stat st;
	char dir[128], path[256];
	uint32_t i;

	make_cfg(&cfg, dir, sizeof(dir), "half");
	CHECK(seglog_open(&sl, &cfg, 0) == 0);
	for (i = 0; i < 5; i++)
		append_sonar(&sl, i, 0);	/* seq 2..6, 35 B each */
	CHECK(seglog_close(&sl) == 0);

	seg_path(path, sizeof(path), dir, 1);
	CHECK(stat(path, &st) == 0);
	CHECK(truncate(path, st.st_size - 10) == 0);	/* 25 B of seq 6 left */

	CHECK(seglog_open(&sl, &cfg, 0) == 0);
	CHECK(sl.truncated_bytes == 25);
	CHECK(seglog_close(&sl) == 0);

	scan_dir(dir, &s);
	CHECK(s.bad == 0 && s.contiguous && s.n == 6);
	CHECK(s.seq[4] == 5);				/* last kept */
	CHECK(s.type[5] == REC_GAP && s.seq[5] == 6);	/* next = last kept + 1 */
	CHECK(s.last_gap_truncated == 25);
}

static void test_rotation_and_cap(void)
{
	struct seglog_cfg cfg;
	struct seglog sl;
	struct scan s;
	struct stat st;
	char dir[128], path[256];
	uint64_t total = 0;
	uint32_t *v, i;
	size_t n, k;

	make_cfg(&cfg, dir, sizeof(dir), "rotate");
	cfg.seg_bytes = 1024;
	cfg.cap_bytes = 3072;
	CHECK(seglog_open(&sl, &cfg, 0) == 0);
	for (i = 0; i < 300; i++)
		append_sonar(&sl, i, 0);	/* ~10.5 KB */

	CHECK(seg_list(dir, &v, &n) == 0);
	CHECK(n >= 3 && n <= 4);
	CHECK(v[0] > 1);			/* oldest were deleted */
	CHECK(v[n - 1] == sl.seg_no);		/* never the current one */
	for (k = 0; k < n; k++) {
		seg_path(path, sizeof(path), dir, v[k]);
		CHECK(stat(path, &st) == 0 && st.st_size <= 1024);
		total += (uint64_t)st.st_size;
	}
	free(v);
	CHECK(total == sl.total_bytes && total <= cfg.cap_bytes + cfg.seg_bytes);
	CHECK(seglog_close(&sl) == 0);

	scan_dir(dir, &s);
	CHECK(s.bad == 0 && s.contiguous && s.seq[s.n - 1] == 301);
}

static int short_once;

static ssize_t short_write(int fd, const void *buf, size_t n)
{
	if (short_once) {
		short_once = 0;
		return write(fd, buf, n / 2);
	}
	return write(fd, buf, n);
}

static void test_short_write_truncated(void)
{
	struct seglog_cfg cfg;
	struct seglog sl;
	struct scan s;
	struct stat st;
	char dir[128], path[256];

	make_cfg(&cfg, dir, sizeof(dir), "short");
	cfg.write_fn = short_write;
	CHECK(seglog_open(&sl, &cfg, 0) == 0);
	short_once = 1;
	append_sonar(&sl, 0, 0);		/* half lands, is cut off, retried */
	CHECK(short_once == 0 && !sl.state_only && sl.refused == 0);
	CHECK(seglog_close(&sl) == 0);

	seg_path(path, sizeof(path), dir, 1);
	CHECK(stat(path, &st) == 0);
	CHECK(st.st_size == (REC_HDR_LEN + REC_GAP_LEN) + (REC_HDR_LEN + 24));
	scan_dir(dir, &s);
	CHECK(s.bad == 0 && s.contiguous && s.n == 2);
}

static int fail_sonar;

static ssize_t nospc_write(int fd, const void *buf, size_t n)
{
	if (fail_sonar && n > 2 && ((const uint8_t *)buf)[2] == MSG_SONAR) {
		errno = ENOSPC;
		return -1;
	}
	return write(fd, buf, n);
}

static void test_enospc_state_only(void)
{
	struct seglog_cfg cfg;
	struct seglog sl;
	struct scan s;
	struct stat st;
	char dir[128], path[256];
	struct sonar_msg m;
	int64_t t0 = 1000 * MS;

	make_cfg(&cfg, dir, sizeof(dir), "nospc");
	CHECK(seglog_open(&sl, &cfg, 0) == 0);
	CHECK(seglog_close(&sl) == 0);		/* seg 1 now old */
	cfg.write_fn = nospc_write;
	CHECK(seglog_open(&sl, &cfg, 0) == 0);	/* seg 2 current */

	fail_sonar = 1;
	memset(&m, 0, sizeof(m));
	m.type = MSG_SONAR;
	CHECK(seglog_append(&sl, MSG_SONAR, &m, sizeof(m), t0) == 1);
	seg_path(path, sizeof(path), dir, 1);
	CHECK(stat(path, &st) == -1 && errno == ENOENT);	/* oldest deleted first */
	CHECK(sl.state_only && sl.refused == 1);
	CHECK(seglog_append(&sl, MSG_SONAR, &m, sizeof(m), t0 + 1000 * MS) == 1);
	CHECK(sl.refused == 2);
	CHECK(append_alert(&sl, t0 + 2000 * MS) == 0);	/* state still kept */

	fail_sonar = 0;
	CHECK(seglog_append(&sl, MSG_SONAR, &m, sizeof(m), t0 + STATE_ONLY_RETRY_NS - 1) == 1);
	CHECK(seglog_append(&sl, MSG_SONAR, &m, sizeof(m), t0 + STATE_ONLY_RETRY_NS) == 0);
	CHECK(!sl.state_only);
	CHECK(seglog_close(&sl) == 0);

	scan_dir(dir, &s);
	CHECK(s.bad == 0 && s.contiguous && s.n == 3);
	CHECK(s.type[0] == REC_GAP && s.type[1] == MSG_ALERT && s.type[2] == MSG_SONAR);
}

static int64_t fake_now;
static int nsync;
static int64_t sync_at[64];

static int count_sync(int fd)
{
	CHECK(nsync < 64);
	sync_at[nsync++] = fake_now;
	return fdatasync(fd);
}

/* A record every 100 ms for 5 s: the caller is never idle, yet syncs must come
 * ~1 s apart. Leaves nsync/sync_at as of the end of the stream, before close's
 * final flush. */
static void run_stream(const char *test, int alert_at_k)
{
	struct seglog_cfg cfg;
	struct seglog sl;
	char dir[128];
	int k, stream_syncs;

	make_cfg(&cfg, dir, sizeof(dir), test);
	cfg.sync_fn = count_sync;
	nsync = 0;
	fake_now = 0;
	CHECK(seglog_open(&sl, &cfg, 0) == 0);	/* GAP: state sync at 0 */
	for (k = 1; k <= 50; k++) {
		fake_now = k * 100 * MS;
		append_sonar(&sl, (uint32_t)k, fake_now);
		if (k == alert_at_k)
			CHECK(append_alert(&sl, fake_now) == 0);
		CHECK(seglog_maybe_sync(&sl, fake_now) >= 0);
	}
	stream_syncs = nsync;
	CHECK(seglog_close(&sl) == 0);
	nsync = stream_syncs;
}

static void test_steady_stream_syncs_each_second(void)
{
	int i;

	run_stream("stream", 0);
	CHECK(nsync == 6);			/* GAP + 5 timed */
	CHECK(sync_at[0] == 0);
	for (i = 1; i < nsync; i++) {
		CHECK(sync_at[i] - sync_at[i - 1] >= 1000 * MS);
		CHECK(sync_at[i] - sync_at[i - 1] <= 1100 * MS);
	}
}

static void test_state_sync_restarts_window(void)
{
	static const int64_t want_ms[] = { 0, 1000, 2000, 2500, 3500, 4500 };
	size_t i;

	run_stream("stream_alert", 25);		/* ALERT at 2.5 s */
	CHECK(nsync == (int)(sizeof(want_ms) / sizeof(want_ms[0])));
	for (i = 0; i < sizeof(want_ms) / sizeof(want_ms[0]); i++)
		CHECK(sync_at[i] == want_ms[i] * MS);
}

static void test_idle_clean_never_syncs(void)
{
	struct seglog_cfg cfg;
	struct seglog sl;
	char dir[128];

	make_cfg(&cfg, dir, sizeof(dir), "idle");
	cfg.sync_fn = count_sync;
	nsync = 0;
	fake_now = 0;
	CHECK(seglog_open(&sl, &cfg, 0) == 0);	/* GAP: state sync */
	CHECK(nsync == 1 && seglog_sync_due_ns(&sl) == INT64_MAX);

	fake_now = 100 * MS;
	append_sonar(&sl, 1, fake_now);
	CHECK(seglog_sync_due_ns(&sl) == 1000 * MS);
	fake_now = 1100 * MS;
	CHECK(seglog_maybe_sync(&sl, fake_now) == 1);	/* the dirty record flushed */
	CHECK(nsync == 2 && seglog_sync_due_ns(&sl) == INT64_MAX);

	fake_now = 5000 * MS;
	CHECK(seglog_maybe_sync(&sl, fake_now) == 0);
	fake_now = 60000 * MS;
	CHECK(seglog_maybe_sync(&sl, fake_now) == 0);
	CHECK(nsync == 2);
	CHECK(seglog_close(&sl) == 0);
	CHECK(nsync == 2);			/* close had nothing to flush */
}

static void rm_tree(const char *dir)
{
	char path[256];
	uint32_t *v;
	size_t n, i;

	if (seg_list(dir, &v, &n) < 0)
		return;
	for (i = 0; i < n; i++) {
		seg_path(path, sizeof(path), dir, v[i]);
		CHECK(unlink(path) == 0);
	}
	free(v);
	CHECK(rmdir(dir) == 0);
}

int main(void)
{
	static const char *dirs[] = { "reopen", "garbage", "half", "rotate", "short", "nospc",
				      "stream", "stream_alert", "idle" };
	char dir[128];
	size_t i;

	CHECK(mkdtemp(base) != NULL);
	RUN(test_reopen_continues_seq);
	RUN(test_torn_tail_garbage);
	RUN(test_torn_tail_half_record);
	RUN(test_rotation_and_cap);
	RUN(test_short_write_truncated);
	RUN(test_enospc_state_only);
	RUN(test_steady_stream_syncs_each_second);
	RUN(test_state_sync_restarts_window);
	RUN(test_idle_clean_never_syncs);

	for (i = 0; i < sizeof(dirs) / sizeof(dirs[0]); i++) {
		snprintf(dir, sizeof(dir), "%s/%s", base, dirs[i]);
		rm_tree(dir);
	}
	CHECK(rmdir(base) == 0);
	return 0;
}
