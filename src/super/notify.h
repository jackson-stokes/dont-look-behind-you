/* Minimal sd_notify(3): one datagram to $NOTIFY_SOCKET ("/path" or "@abstract").
 * No libsystemd. Returns 0 (also when $NOTIFY_SOCKET is unset), or -1 with errno.
 */
#ifndef DLBY_NOTIFY_H
#define DLBY_NOTIFY_H

#define NOTIFY_SOCKET_ENV "NOTIFY_SOCKET"

int notify_send(const char *msg);

#endif
