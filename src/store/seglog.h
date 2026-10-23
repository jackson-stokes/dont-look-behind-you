/* Crash-consistent append-only segment log (DESIGN.md §2 D, §4). No sockets, so
 * it is unit-tested directly; store.c feeds it.
 *
 * - Segments <dir>/seg-NNNNNN.log, each record one write(); a failed or short
 *   write is truncated away so a live segment never holds a partial record.
 * - fdatasync after every state record (ALERT, GAP) and at least every SYNC_NS
 *   while dirty, via seglog_maybe_sync() on every pass of the caller's loop.
 * - Open truncates a torn tail, continues seq from the last valid record, starts
 *   a new segment and writes a REC_GAP record with the truncated byte count.
 * - Over cap_bytes, or on ENOSPC, the oldest segment is deleted. If ENOSPC
 *   persists, only state records are kept until a retry 60 s later.
 */
#ifndef DLBY_SEGLOG_H
#define DLBY_SEGLOG_H

#include "log.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>

#define LOG_DIR_DEFAULT     "/var/lib/dlby/log"
#define LOG_DIR_ENV         "DLBY_LOG_DIR"
#define SEG_BYTES_DEFAULT   (8ull << 20)
#define SEG_CAP_DEFAULT     (512ull << 20)
#define SYNC_NS             1000000000LL
#define STATE_ONLY_RETRY_NS (60LL * 1000000000LL)

/* Test seams; NULL means write(2) and fdatasync(2). */
typedef ssize_t (*seglog_write_fn)(int fd, const void *buf, size_t n);
typedef int (*seglog_sync_fn)(int fd);

struct seglog_cfg {
	const char     *dir;
	uint64_t        seg_bytes;
	uint64_t        cap_bytes;
	seglog_write_fn write_fn;
	seglog_sync_fn  sync_fn;
};

struct seglog {
	struct seglog_cfg cfg;
	char     dir[200];
	int      dfd;		/* the directory, for fsync after create/unlink */
	int      fd;		/* current segment */
	uint32_t seg_no;
	char     seg_name[32];
	uint64_t seg_size;
	uint64_t total_bytes;
	uint32_t next_seq;
	uint32_t truncated_bytes;	/* by recovery at open */
	bool     dirty;
	int64_t  last_sync_ns;
	bool     state_only;
	int64_t  state_only_since_ns;
	uint64_t records, refused, syncs_timed, syncs_state;
	struct log_limit lim_rotate;
};

void seglog_default_cfg(struct seglog_cfg *cfg);

int seglog_open(struct seglog *sl, const struct seglog_cfg *cfg, int64_t now_ns);

/* Returns 0 if written, 1 if refused (state_only mode), -1 with errno on error.
 * ENOSPC from -1 means even a state record didn't fit; anything else is fatal. */
int seglog_append(struct seglog *sl, uint8_t type, const void *payload, size_t len, int64_t now_ns);

/* fdatasync if dirty and SYNC_NS has passed since the last sync. Returns 1 if it
 * synced, 0 if not due, -1 on error. Call on every loop pass, not only on idle. */
int seglog_maybe_sync(struct seglog *sl, int64_t now_ns);

/* When seglog_maybe_sync() will next have work; INT64_MAX if nothing is dirty. */
int64_t seglog_sync_due_ns(const struct seglog *sl);

/* Final fdatasync and close. Returns 0, or -1 if the sync or a close failed. */
int seglog_close(struct seglog *sl);

/* Segment numbers in dir, ascending, in a malloc'd array (free it). */
int seg_list(const char *dir, uint32_t **nos, size_t *n);
void seg_name(char *buf, size_t cap, uint32_t no);

#endif
