#include "check.h"
#include "lathist.h"

#include <fcntl.h>
#include <unistd.h>

#define US 1000LL
#define MS 1000000LL

static struct lathist h;	/* ~27 KB: off the stack */

static void test_bins(void)
{
	/* Each region's first and last bin, and its edges. */
	CHECK(lathist_bin(0) == 0 && lathist_bin(999) == 0 && lathist_bin(US) == 1);
	CHECK(lathist_bin(2 * MS - 1) == 1999);
	CHECK(lathist_bin(2 * MS) == 2000 && lathist_bin_lo(2000) == 2 * MS);
	CHECK(lathist_bin_hi(2000) == 2 * MS + 50 * US);
	CHECK(lathist_bin(20 * MS - 1) == 2359);
	CHECK(lathist_bin(20 * MS) == 2360 && lathist_bin_lo(2360) == 20 * MS);
	CHECK(lathist_bin_hi(2360) == 21 * MS);
	CHECK(lathist_bin(1000 * MS - 1) == 3339);
	CHECK(lathist_bin(1000 * MS) == 3340 && lathist_bin(3600LL * 1000 * MS) == 3340);
	CHECK(lathist_bin_lo(3340) == 1000 * MS && lathist_bin_hi(3340) == INT64_MAX);
	/* Bins tile the line: each starts where the last ended. */
	for (int i = 0; i + 1 < LATHIST_NBINS; i++)
		CHECK(lathist_bin_hi(i) == lathist_bin_lo(i + 1) && lathist_bin_lo(i) < lathist_bin_hi(i));
}

static void test_empty(void)
{
	char buf[256];

	lathist_init(&h);
	CHECK(lathist_quantile(&h, 0.5) == 0 && lathist_quantile(&h, 0.99) == 0);
	lathist_fmt(&h, buf, sizeof(buf));
	CHECK(strcmp(buf, "n=0 p50_us=0.0 p90_us=0.0 p99_us=0.0 p999_us=0.0 max_us=0.0"
			  " mean_us=0.0 neg=0") == 0);
}

/* 1..1000 µs, one each: quantiles land within one 1 µs bin of the true value. */
static void test_uniform_quantiles(void)
{
	int64_t q;
	int k;

	lathist_init(&h);
	for (k = 1; k <= 1000; k++)
		lathist_add(&h, k * US);
	CHECK(h.count == 1000 && h.max_ns == 1000 * US && h.neg == 0);
	q = lathist_quantile(&h, 0.50);
	CHECK(q >= 500 * US && q <= 501 * US);
	q = lathist_quantile(&h, 0.99);
	CHECK(q >= 990 * US && q <= 991 * US);
	CHECK(lathist_quantile(&h, 1.0) == 1000 * US);		/* capped at the true max */
	CHECK(h.sum_ns == 500500 * US);				/* mean 500.5 µs */
}

static void test_single_and_ties(void)
{
	char buf[256];

	lathist_init(&h);
	lathist_add(&h, 7 * MS);
	/* One 7 ms sample: every quantile is it (bin hi capped at max). */
	CHECK(lathist_quantile(&h, 0.5) == 7 * MS && lathist_quantile(&h, 0.999) == 7 * MS);
	lathist_init(&h);
	for (int i = 0; i < 99; i++)
		lathist_add(&h, 300 * US);
	lathist_add(&h, 15 * MS);				/* one outlier in 100 */
	CHECK(lathist_quantile(&h, 0.99) <= 301 * US);		/* rank 99 is still a tie */
	CHECK(lathist_quantile(&h, 0.999) == 15 * MS);
	lathist_fmt(&h, buf, sizeof(buf));
	CHECK(strstr(buf, "n=100 ") && strstr(buf, " max_us=15000.0 ") && strstr(buf, " neg=0"));
}

static void test_negative_and_overflow(void)
{
	lathist_init(&h);
	lathist_add(&h, -5);
	lathist_add(&h, -1 * MS);
	CHECK(h.neg == 2 && h.count == 0 && lathist_quantile(&h, 0.5) == 0);
	lathist_add(&h, 5LL * 1000 * MS);			/* 5 s: overflow bin */
	CHECK(h.count == 1 && h.bins[LATHIST_NBINS - 1] == 1);
	CHECK(lathist_quantile(&h, 0.5) == 5LL * 1000 * MS);	/* the true max, not INT64_MAX */
}

/* The dump's bin counts add up to n, and its header carries n. */
static void test_write_dump(void)
{
	char dir[] = "/tmp/dlby-lathist-XXXXXX", path[256], line[128];
	unsigned long long lo, hi, cnt, total = 0;
	FILE *f;
	int k;

	lathist_init(&h);
	for (k = 0; k < 500; k++)
		lathist_add(&h, (k % 50) * 100 * US + 3);	/* 0..4.9 ms, two regions */
	lathist_add(&h, 2LL * 1000 * MS);
	lathist_add(&h, -7);
	CHECK(mkdtemp(dir) != NULL);
	CHECK(lathist_dump(&h, dir, "alert", "origin_gpio") == 0);
	snprintf(path, sizeof(path), "%s/alert-origin_gpio-%ld.hist", dir, (long)getpid());
	f = fopen(path, "r");
	CHECK(f != NULL);
	CHECK(fgets(line, sizeof(line), f) != NULL);
	CHECK(strncmp(line, "# lathist name=origin_gpio n=501 ", 33) == 0 && strstr(line, " neg=1\n"));
	while (fgets(line, sizeof(line), f)) {
		CHECK(sscanf(line, "%llu %llu %llu", &lo, &hi, &cnt) == 3 && lo < hi && cnt > 0);
		total += cnt;
	}
	CHECK(fclose(f) == 0);
	CHECK(total == 501);
	CHECK(unlink(path) == 0 && rmdir(dir) == 0);
	errno = 0;
	CHECK(lathist_dump(&h, "/nonexistent-dir", "alert", "x") == -1 && errno == ENOENT);
	CHECK(lathist_write(&h, -1, "x") == -1 && errno == EBADF);
}

int main(void)
{
	RUN(test_bins);
	RUN(test_empty);
	RUN(test_uniform_quantiles);
	RUN(test_single_and_ties);
	RUN(test_negative_and_overflow);
	RUN(test_write_dump);
	return 0;
}
