/* Must FAIL tools/check-boundary.sh: name resolution. Never compiled. */
#include <netdb.h>

int f(struct addrinfo **res) { return getaddrinfo("example.org", "80", 0, res); }
