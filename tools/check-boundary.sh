#!/bin/sh
# Text check for the no-network boundary (DESIGN.md §8). Usage: check-boundary.sh <dir|file>...
# It can miss things; systemd's RestrictAddressFamilies=AF_UNIX is the real enforcement.
set -u

status=0
banned='AF_INET6?|PF_INET6?|getaddrinfo|gethostbyname|<netdb\.h>|<netinet/|<arpa/inet\.h>|\bcurl|libsoup|microhttpd|<http'

for target in "$@"; do
	if grep -rnE --include='*.c' --include='*.h' "$banned" "$target"; then
		status=1
	fi
	# connect() is only allowed in files that use AF_UNIX.
	for f in $(grep -rlE --include='*.c' --include='*.h' '\bconnect[[:space:]]*\(' "$target"); do
		if ! grep -q 'AF_UNIX' "$f"; then
			echo "$f: connect() in a file without AF_UNIX"
			status=1
		fi
	done
done

if [ "$status" -ne 0 ]; then
	echo "check-boundary: FAILED (network use in $*)"
fi
exit "$status"
