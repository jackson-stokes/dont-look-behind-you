/* Must FAIL tools/check-boundary.sh: connect() in a file that never names the unix family. Never compiled. */
#include <sys/socket.h>

int f(int fd, const struct sockaddr *sa, socklen_t len) { return connect(fd, sa, len); }
