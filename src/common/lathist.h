/* Latency histograms (DESIGN.md §6, mechanism C). Fixed bins, no allocation, so
 * a real-time process can record into one on every message:
 *
 *   [0, 2 ms)     1 µs bins     2000 bins
 *   [2, 20 ms)    50 µs bins     360 bins
 *   [20 ms, 1 s)  1 ms bins      980 bins
 *   >= 1 s        one overflow bin
 *
 * Exact count, sum (for the mean) and max are kept beside the bins. A negative
 * sample means a bogus origin timestamp: it is counted in neg, never binned.
 * Quantiles are the upper edge of the bin holding that rank, capped at the true
 * max, so they are never more than one bin width too high.
 *
 * Every timestamp involved is CLOCK_MONOTONIC, the same clock in every process.
 */
#ifndef DLBY_LATHIST_H
#define DLBY_LATHIST_H

#include <stddef.h>
#include <stdint.h>

#define LATHIST_NBINS  3341
#define LATHIST_DIR_ENV "DLBY_LAT_DIR"

struct lathist {
	uint64_t bins[LATHIST_NBINS];
	uint64_t count;		/* binned samples (neg not included) */
	uint64_t neg;
	uint64_t sum_ns;
	uint64_t max_ns;
};

void lathist_init(struct lathist *h);
void lathist_add(struct lathist *h, int64_t ns);

/* The bin a sample lands in, and that bin's [lo, hi) edges in ns. */
int lathist_bin(int64_t ns);
int64_t lathist_bin_lo(int bin);
int64_t lathist_bin_hi(int bin);

/* q in (0, 1]: the latency at or below which a fraction q of samples lie.
 * 0 for an empty histogram. */
int64_t lathist_quantile(const struct lathist *h, double q);

/* "n=… p50_us=… p90_us=… p99_us=… p999_us=… max_us=… mean_us=… neg=…" */
void lathist_fmt(const struct lathist *h, char *buf, size_t cap);

/* Text dump: "# lathist name=<name> n=… max_ns=… sum_ns=… neg=…", then
 * "<lo_ns> <hi_ns> <count>" for every non-empty bin. Returns 0, or -1 with errno. */
int lathist_write(const struct lathist *h, int fd, const char *name);

/* lathist_write to <dir>/<proc>-<name>-<pid>.hist. Returns 0, or -1 with errno. */
int lathist_dump(const struct lathist *h, const char *dir, const char *proc, const char *name);

#endif
