/* Unix-domain SOCK_SEQPACKET links between dlby processes (DESIGN.md §1).
 *
 * Receivers listen on <dir>/<name>.sock, where <dir> is $DLBY_RUN_DIR or
 * /run/dlby. Senders hold a struct ipc_conn and call ipc_send(), which never
 * blocks: while the receiver is away it reconnects with backoff (50 ms doubling
 * to 1 s) and drops (and counts) messages meanwhile. Every fd is close-on-exec.
 * Functions return -1 with errno set on failure.
 */
#ifndef DLBY_IPC_H
#define DLBY_IPC_H

#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>

#define IPC_DEFAULT_DIR "/run/dlby"
#define IPC_DIR_ENV     "DLBY_RUN_DIR"
#define IPC_PATH_MAX    108	/* sizeof(sockaddr_un.sun_path) on Linux */

const char *ipc_dir(void);
int ipc_path(const char *name, char *buf, size_t cap);

/* Create the dir (0700) if needed, replace a stale socket (never a non-socket),
 * bind and listen. Returns the listening fd. */
int ipc_listen(const char *name);
int ipc_accept(int lfd);

struct ipc_conn {
	char     path[IPC_PATH_MAX];
	int      fd;		/* -1 while disconnected; poll it for POLLHUP */
	int      backoff_ms;
	int64_t  next_try_ns;
	uint64_t dropped;
};

int  ipc_conn_init(struct ipc_conn *c, const char *name);
void ipc_conn_close(struct ipc_conn *c);

/* Send one message (1..MSG_MAX bytes). Returns 0, or -1 with the message dropped
 * and c->dropped incremented: ENOTCONN while waiting out the backoff, the connect
 * error on a failed attempt, EAGAIN if the receiver is slow, EPIPE/ECONNRESET if
 * it died (the connection is closed and retried on the next send). */
int ipc_send(struct ipc_conn *c, const void *buf, size_t len, int64_t now_ns);

/* Receive one message into buf (cap >= MSG_MAX, else EINVAL). Returns its length,
 * or 0 on EOF (senders never send empty messages). An oversized message is
 * consumed, rejected with EMSGSIZE and counted in *rejected. */
ssize_t ipc_recv(int fd, void *buf, size_t cap, uint64_t *rejected);

/* Check a received message's length against its type. Returns the type, or -1
 * with EBADMSG (counted in *rejected) for a short, unknown or wrong-length one. */
int ipc_check_msg(const void *buf, size_t len, uint64_t *rejected);

#endif
