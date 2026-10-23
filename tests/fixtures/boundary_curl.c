/* Must FAIL tools/check-boundary.sh: an HTTP client library. Never compiled. */
#include <curl/curl.h>

void *f(void) { return curl_easy_init(); }
