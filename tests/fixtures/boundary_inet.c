/* Must FAIL tools/check-boundary.sh: an inet socket. Never compiled. */
#include <sys/socket.h>

int f(void) { return socket(AF_INET6, SOCK_STREAM, 0); }
