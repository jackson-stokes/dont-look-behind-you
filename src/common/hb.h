/* Child side of dlby-super's heartbeat (DESIGN.md §2 E).
 *
 * dlby-super passes one end of a SOCK_SEQPACKET socketpair as $DLBY_HB_FD. A
 * child sends a struct hb_msg every HB_PERIOD_NS from its main event loop, never
 * a side thread, so a stuck loop goes quiet. EOF on the fd means super is gone:
 * the child exits. hb_init also sets PR_SET_PDEATHSIG(SIGTERM).
 *
 * Without $DLBY_HB_FD the child runs standalone (bench, tests): hb_tick and
 * hb_readable do nothing and hb_due_ns is INT64_MAX.
 */
#ifndef DLBY_HB_H
#define DLBY_HB_H

#include <stdint.h>

#define HB_FD_ENV        "DLBY_HB_FD"
#define HB_SUPER_PID_ENV "DLBY_SUPER_PID"
#define HB_PERIOD_NS     1000000000LL

struct hb {
	int      fd;		/* -1 when standalone; poll it for POLLIN/POLLHUP */
	uint32_t seq;
	int64_t  next_ns;
};

/* Returns 1 if supervised, 0 if standalone, -1 with errno on error (ESRCH:
 * super already died before PDEATHSIG was armed). */
int hb_init(struct hb *hb);

/* Send a heartbeat if one is due. Returns 0, or -1 if super is gone. */
int hb_tick(struct hb *hb, int64_t now_ns);

/* Call when hb->fd is readable. Returns 0, or -1 on EOF (super is gone). */
int hb_readable(struct hb *hb);

int64_t hb_due_ns(const struct hb *hb);

#endif
