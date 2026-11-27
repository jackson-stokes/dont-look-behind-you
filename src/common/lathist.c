#include "lathist.h"

#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#define US 1000LL
#define MS 1000000LL

#define A_END  (2 * MS)		/* 1 µs bins below */
#define B_END  (20 * MS)	/* 50 µs bins below */
#define C_END  (1000 * MS)	/* 1 ms bins below */
#define A_BINS 2000
#define B_BINS 360
#define C_BINS 980
#define OVERFLOW (A_BINS + B_BINS + C_BINS)

_Static_assert(OVERFLOW + 1 == LATHIST_NBINS, "bin count");

void lathist_init(struct lathist *h)
{
	memset(h, 0, sizeof(*h));
}

int lathist_bin(int64_t ns)
{
	if (ns < A_END)
		return (int)(ns / US);
	if (ns < B_END)
		return A_BINS + (int)((ns - A_END) / (50 * US));
	if (ns < C_END)
		return A_BINS + B_BINS + (int)((ns - B_END) / MS);
	return OVERFLOW;
}

int64_t lathist_bin_lo(int bin)
{
	if (bin < A_BINS)
		return (int64_t)bin * US;
	if (bin < A_BINS + B_BINS)
		return A_END + (int64_t)(bin - A_BINS) * 50 * US;
	if (bin < OVERFLOW)
		return B_END + (int64_t)(bin - A_BINS - B_BINS) * MS;
	return C_END;
}

int64_t lathist_bin_hi(int bin)
{
	return bin < OVERFLOW ? lathist_bin_lo(bin + 1) : INT64_MAX;
}

void lathist_add(struct lathist *h, int64_t ns)
{
	if (ns < 0) {
		h->neg++;
		return;
	}
	h->bins[lathist_bin(ns)]++;
	h->count++;
	h->sum_ns += (uint64_t)ns;
	if ((uint64_t)ns > h->max_ns)
		h->max_ns = (uint64_t)ns;
}

int64_t lathist_quantile(const struct lathist *h, double q)
{
	uint64_t target, seen = 0;
	int64_t hi;
	int i;

	if (h->count == 0)
		return 0;
	target = (uint64_t)(q * (double)h->count);
	if ((double)target < q * (double)h->count)
		target++;		/* ceil */
	if (target < 1)
		target = 1;
	if (target > h->count)
		target = h->count;
	for (i = 0; i < LATHIST_NBINS; i++) {
		seen += h->bins[i];
		if (seen >= target) {
			hi = lathist_bin_hi(i);
			return hi < (int64_t)h->max_ns ? hi : (int64_t)h->max_ns;
		}
	}
	return (int64_t)h->max_ns;	/* not reached: the bins sum to count */
}

void lathist_fmt(const struct lathist *h, char *buf, size_t cap)
{
	double mean = h->count ? (double)h->sum_ns / (double)h->count : 0.0;

	snprintf(buf, cap, "n=%" PRIu64 " p50_us=%.1f p90_us=%.1f p99_us=%.1f p999_us=%.1f"
		 " max_us=%.1f mean_us=%.1f neg=%" PRIu64, h->count,
		 (double)lathist_quantile(h, 0.50) / US, (double)lathist_quantile(h, 0.90) / US,
		 (double)lathist_quantile(h, 0.99) / US, (double)lathist_quantile(h, 0.999) / US,
		 (double)h->max_ns / US, mean / US, h->neg);
}

/* All of buf, or -1 with errno. */
static int write_all(int fd, const char *buf, size_t n)
{
	ssize_t w;

	while (n > 0) {
		w = write(fd, buf, n);
		if (w < 0) {
			if (errno == EINTR)
				continue;
			return -1;
		}
		buf += w;
		n -= (size_t)w;
	}
	return 0;
}

int lathist_write(const struct lathist *h, int fd, const char *name)
{
	char line[128];
	int i, n;

	n = snprintf(line, sizeof(line), "# lathist name=%s n=%" PRIu64 " max_ns=%" PRIu64
		     " sum_ns=%" PRIu64 " neg=%" PRIu64 "\n", name, h->count, h->max_ns, h->sum_ns,
		     h->neg);
	if (n < 0 || (size_t)n >= sizeof(line)) {
		errno = EINVAL;
		return -1;
	}
	if (write_all(fd, line, (size_t)n) < 0)
		return -1;
	for (i = 0; i < LATHIST_NBINS; i++) {
		if (!h->bins[i])
			continue;
		n = snprintf(line, sizeof(line), "%" PRId64 " %" PRId64 " %" PRIu64 "\n",
			     lathist_bin_lo(i), lathist_bin_hi(i), h->bins[i]);
		if (write_all(fd, line, (size_t)n) < 0)
			return -1;
	}
	return 0;
}

int lathist_dump(const struct lathist *h, const char *dir, const char *proc, const char *name)
{
	char path[512];
	int fd, n, err;

	n = snprintf(path, sizeof(path), "%s/%s-%s-%ld.hist", dir, proc, name, (long)getpid());
	if (n < 0 || (size_t)n >= sizeof(path)) {
		errno = ENAMETOOLONG;
		return -1;
	}
	fd = open(path, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
	if (fd < 0)
		return -1;
	if (lathist_write(h, fd, name) < 0) {
		err = errno;
		(void)close(fd);	/* released even if close reports an error */
		errno = err;
		return -1;
	}
	return close(fd);
}
