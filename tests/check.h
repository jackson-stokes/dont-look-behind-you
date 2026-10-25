/* Minimal test helpers: a failed CHECK prints where and exits non-zero.
 * Failures go to stdout because test_log redirects stderr into a pipe. */
#ifndef DLBY_CHECK_H
#define DLBY_CHECK_H

#include <errno.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CHECK(cond) do { \
	if (!(cond)) { \
		int e_ = errno; \
		printf("%s:%d: CHECK(%s) failed (errno=%d %s)\n", \
		       __FILE__, __LINE__, #cond, e_, strerror(e_)); \
		fflush(stdout); \
		exit(1); \
	} \
} while (0)

#define RUN(fn) do { fn(); printf("ok %s\n", #fn); fflush(stdout); } while (0)

#endif
