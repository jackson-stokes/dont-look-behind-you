#include "seglog.h"
#include "msg.h"
#include "record.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

struct last_seq {
	bool     found;
	uint32_t seq;
};

static bool is_state_type(uint8_t type)
{
	return type == MSG_ALERT || type == MSG_GAP || type == REC_GAP;
}

/* close() releases the fd on Linux even on error; keep the caller's errno. */
static void close_keep_errno(int fd)
{
	int err = errno;

	(void)close(fd);
	errno = err;
}

void seg_name(char *buf, size_t cap, uint32_t no)
{
	snprintf(buf, cap, "seg-%06" PRIu32 ".log", no);
}

static bool parse_seg(const char *name, uint32_t *no)
{
	unsigned long v;
	char *end;

	if (strncmp(name, "seg-", 4) != 0 || name[4] < '0' || name[4] > '9')
		return false;
	errno = 0;
	v = strtoul(name + 4, &end, 10);
	if (errno || v == 0 || v > UINT32_MAX || strcmp(end, ".log") != 0)
		return false;
	*no = (uint32_t)v;
	return true;
}

static int cmp_u32(const void *a, const void *b)
{
	uint32_t x = *(const uint32_t *)a, y = *(const uint32_t *)b;

	return x < y ? -1 : x > y;
}

int seg_list(const char *dir, uint32_t **nos, size_t *n)
{
	uint32_t *v = NULL, *nv, no;
	size_t len = 0, cap = 0;
	struct dirent *de;
	DIR *d;
	int err;

	*nos = NULL;
	*n = 0;
	d = opendir(dir);
	if (!d)
		return -1;
	for (;;) {
		errno = 0;
		de = readdir(d);
		if (!de) {
			if (errno)
				goto fail;
			break;
		}
		if (!parse_seg(de->d_name, &no))
			continue;
		if (len == cap) {
			cap = cap ? cap * 2 : 16;
			nv = realloc(v, cap * sizeof(*v));
			if (!nv)
				goto fail;
			v = nv;
		}
		v[len++] = no;
	}
	if (closedir(d) < 0) {
		d = NULL;
		goto fail;
	}
	if (len)
		qsort(v, len, sizeof(*v), cmp_u32);
	*nos = v;
	*n = len;
	return 0;

fail:
	err = errno;
	if (d)
		(void)closedir(d);
	free(v);
	errno = err;
	return -1;
}

static int mkdir_p(const char *path)
{
	char tmp[sizeof(((struct seglog *)0)->dir)];
	char *p;

	snprintf(tmp, sizeof(tmp), "%s", path);
	for (p = tmp + 1; *p; p++) {
		if (*p != '/')
			continue;
		*p = '\0';
		if (mkdir(tmp, 0700) < 0 && errno != EEXIST)
			return -1;
		*p = '/';
	}
	if (mkdir(tmp, 0700) < 0 && errno != EEXIST)
		return -1;
	return 0;
}

static int sync_now(struct seglog *sl, int64_t now_ns)
{
	if (!sl->dirty)
		return 0;
	if (sl->cfg.sync_fn(sl->fd) < 0)
		return -1;
	sl->dirty = false;
	sl->last_sync_ns = now_ns;
	return 1;
}

static int open_segment(struct seglog *sl, uint32_t no)
{
	char name[32];
	int fd;

	seg_name(name, sizeof(name), no);
	fd = openat(sl->dfd, name, O_WRONLY | O_CREAT | O_EXCL | O_APPEND | O_CLOEXEC, 0600);
	if (fd < 0)
		return -1;
	if (fsync(sl->dfd) < 0) {
		close_keep_errno(fd);
		return -1;
	}
	sl->fd = fd;
	sl->seg_no = no;
	sl->seg_size = 0;
	memcpy(sl->seg_name, name, sizeof(name));
	return 0;
}

/* Delete the oldest segment other than the current one. -1 with ENOSPC if none. */
static int delete_oldest(struct seglog *sl, const char *reason)
{
	char name[32];
	struct stat st;
	uint32_t *v;
	size_t n;
	int rc = -1, err;

	if (seg_list(sl->dir, &v, &n) < 0)
		return -1;
	if (n < 2 || v[0] == sl->seg_no) {
		errno = ENOSPC;
		goto out;
	}
	seg_name(name, sizeof(name), v[0]);
	if (fstatat(sl->dfd, name, &st, 0) < 0 || unlinkat(sl->dfd, name, 0) < 0 || fsync(sl->dfd) < 0)
		goto out;
	sl->total_bytes -= (uint64_t)st.st_size < sl->total_bytes ? (uint64_t)st.st_size : sl->total_bytes;
	log_msg(LOG_I, "segment_deleted name=%s reason=%s", name, reason);
	rc = 0;
out:
	err = errno;
	free(v);
	errno = err;
	return rc;
}

static void enforce_cap(struct seglog *sl)
{
	while (sl->total_bytes > sl->cfg.cap_bytes)
		if (delete_oldest(sl, "cap") < 0)
			break;
}

/* A failed open of the next segment keeps the current one (oversized but valid). */
static int rotate(struct seglog *sl, int64_t now_ns)
{
	int old = sl->fd;

	if (sync_now(sl, now_ns) < 0)
		return -1;
	if (open_segment(sl, sl->seg_no + 1) < 0) {
		log_limited(&sl->lim_rotate, now_ns, LOG_W, "rotate_failed err=%s", strerror(errno));
		return 0;
	}
	if (close(old) < 0)
		log_msg(LOG_W, "close_failed err=%s", strerror(errno));
	enforce_cap(sl);
	return 0;
}

/* One write per record. A short or failed write is cut back off the file. */
static int write_record(struct seglog *sl, const uint8_t *buf, size_t n)
{
	ssize_t w;
	int err;

	do {
		w = sl->cfg.write_fn(sl->fd, buf, n);
	} while (w < 0 && errno == EINTR);
	if (w == (ssize_t)n) {
		sl->seg_size += n;
		sl->total_bytes += n;
		sl->dirty = true;
		return 0;
	}
	err = w < 0 ? errno : ENOSPC;	/* a short file write means out of space */
	if (ftruncate(sl->fd, (off_t)sl->seg_size) < 0) {
		log_msg(LOG_E, "truncate_failed err=%s", strerror(errno));
		errno = EIO;
		return -1;
	}
	errno = err;
	return -1;
}

int seglog_append(struct seglog *sl, uint8_t type, const void *payload, size_t len, int64_t now_ns)
{
	uint8_t buf[REC_HDR_LEN + REC_MAX_PAYLOAD];
	bool state = is_state_type(type);
	size_t n;
	int r;

	if (len == 0 || len > REC_MAX_PAYLOAD) {
		errno = EINVAL;
		return -1;
	}
	if (sl->state_only && !state) {
		if (now_ns - sl->state_only_since_ns < STATE_ONLY_RETRY_NS) {
			sl->refused++;
			return 1;
		}
		sl->state_only = false;
		log_msg(LOG_I, "enospc_retry mode=full");
	}

	n = rec_encode(buf, type, sl->next_seq, payload, len);
	if (sl->seg_size > 0 && sl->seg_size + n > sl->cfg.seg_bytes && rotate(sl, now_ns) < 0)
		return -1;

	r = write_record(sl, buf, n);
	if (r < 0 && errno == ENOSPC) {
		(void)delete_oldest(sl, "enospc");
		r = write_record(sl, buf, n);
		if (r < 0 && errno == ENOSPC && !state) {
			sl->state_only = true;
			sl->state_only_since_ns = now_ns;
			sl->refused++;
			log_msg(LOG_E, "enospc_persist mode=state_only");
			return 1;
		}
	}
	if (r < 0)
		return -1;

	sl->next_seq++;
	sl->records++;
	if (state) {
		if (sync_now(sl, now_ns) < 0)
			return -1;
		sl->syncs_state++;
	}
	return 0;
}

int seglog_maybe_sync(struct seglog *sl, int64_t now_ns)
{
	int r;

	if (!sl->dirty || now_ns - sl->last_sync_ns < SYNC_NS)
		return 0;
	r = sync_now(sl, now_ns);
	if (r > 0)
		sl->syncs_timed++;
	return r;
}

int64_t seglog_sync_due_ns(const struct seglog *sl)
{
	return sl->dirty ? sl->last_sync_ns + SYNC_NS : INT64_MAX;
}

static int last_seq_cb(void *ctx, const struct rec *r, const uint8_t *payload)
{
	struct last_seq *ls = ctx;

	(void)payload;
	ls->found = true;
	ls->seq = r->seq;
	return 0;
}

void seglog_default_cfg(struct seglog_cfg *cfg)
{
	const char *d = getenv(LOG_DIR_ENV);

	memset(cfg, 0, sizeof(*cfg));
	cfg->dir = (d && *d) ? d : LOG_DIR_DEFAULT;
	cfg->seg_bytes = SEG_BYTES_DEFAULT;
	cfg->cap_bytes = SEG_CAP_DEFAULT;
}

int seglog_open(struct seglog *sl, const struct seglog_cfg *cfg, int64_t now_ns)
{
	struct last_seq ls = { false, 0 };
	uint8_t gap[REC_GAP_LEN];
	char name[32];
	struct stat st;
	uint32_t *v = NULL;
	off_t good, end;
	size_t n = 0, i;
	int fd = -1, err;

	memset(sl, 0, sizeof(*sl));
	sl->fd = -1;
	sl->dfd = -1;
	sl->cfg = *cfg;
	if (!sl->cfg.write_fn)
		sl->cfg.write_fn = write;
	if (!sl->cfg.sync_fn)
		sl->cfg.sync_fn = fdatasync;
	if (strlen(cfg->dir) >= sizeof(sl->dir)) {
		errno = ENAMETOOLONG;
		return -1;
	}
	memcpy(sl->dir, cfg->dir, strlen(cfg->dir) + 1);
	if (mkdir_p(sl->dir) < 0)
		return -1;
	sl->dfd = open(sl->dir, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
	if (sl->dfd < 0)
		goto fail;
	if (seg_list(sl->dir, &v, &n) < 0)
		goto fail;

	/* Only the newest segment can have a torn tail; older ones were synced
	 * before rotation. Walk back until a valid record gives last seq. */
	for (i = n; i-- > 0;) {
		seg_name(name, sizeof(name), v[i]);
		fd = openat(sl->dfd, name, (i == n - 1 ? O_RDWR : O_RDONLY) | O_CLOEXEC);
		if (fd < 0 || rec_scan(fd, last_seq_cb, &ls, &good, &end) < 0)
			goto fail;
		if (i == n - 1 && good < end) {
			if (ftruncate(fd, good) < 0 || fsync(fd) < 0)
				goto fail;
			sl->truncated_bytes = (uint32_t)(end - good);
			log_msg(LOG_W, "torn_tail truncated_bytes=%" PRIu32, sl->truncated_bytes);
		}
		if (close(fd) < 0) {
			fd = -1;
			goto fail;
		}
		fd = -1;
		if (ls.found)
			break;
	}
	for (i = 0; i < n; i++) {
		seg_name(name, sizeof(name), v[i]);
		if (fstatat(sl->dfd, name, &st, 0) < 0)
			goto fail;
		sl->total_bytes += (uint64_t)st.st_size;
	}

	sl->next_seq = ls.found ? ls.seq + 1 : 1;
	if (open_segment(sl, n ? v[n - 1] + 1 : 1) < 0)
		goto fail;
	free(v);
	v = NULL;

	sl->last_sync_ns = now_ns;
	put_le32(gap, sl->truncated_bytes);
	put_le32(gap + 4, 0);
	if (seglog_append(sl, REC_GAP, gap, sizeof(gap), now_ns) != 0)
		goto fail;
	enforce_cap(sl);
	return 0;

fail:
	err = errno;
	if (fd >= 0)
		(void)close(fd);
	free(v);
	if (sl->fd >= 0)
		(void)close(sl->fd);
	if (sl->dfd >= 0)
		(void)close(sl->dfd);
	sl->fd = -1;
	sl->dfd = -1;
	errno = err;
	return -1;
}

int seglog_close(struct seglog *sl)
{
	int rc = 0;

	if (sl->fd >= 0) {
		if (sync_now(sl, sl->last_sync_ns) < 0)
			rc = -1;
		if (close(sl->fd) < 0)
			rc = -1;
		sl->fd = -1;
	}
	if (sl->dfd >= 0) {
		if (close(sl->dfd) < 0)
			rc = -1;
		sl->dfd = -1;
	}
	return rc;
}
