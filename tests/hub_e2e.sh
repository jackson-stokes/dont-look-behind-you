#!/bin/sh
# End-to-end: fuse-hub plus two fake senders at the real rates; checks the hub's
# DESIGN.md §3 log lines and exit code. Usage: hub_e2e.sh <builddir>
#
#   radar sends for 3 s, then exits              -> EOF at ~3 s
#   sonar sends for 1 s, then holds silent 2 s   -> stale at ~1.44 s
set -u
B=$1
T=$(mktemp -d /tmp/dlby-e2e-XXXXXX) || exit 1
export DLBY_RUN_DIR="$T/run"
LOG="$T/hub.log"
hub=""
radar=""
sonar=""

cleanup() {
	for p in $hub $radar $sonar; do
		kill -KILL "$p" 2>/dev/null
	done
	rm -rf "$T"
}
trap cleanup EXIT

fail() {
	echo "hub_e2e: FAIL: $*"
	for f in "$LOG" "$T/radar.log" "$T/sonar.log"; do
		echo "---- $f"
		cat "$f" 2>/dev/null
	done
	exit 1
}

line_of() {
	grep -n -E "$1" "$LOG" | head -n 1 | cut -d: -f1
}

"$B/fuse-hub" 2>"$LOG" &
hub=$!
i=0
while [ ! -S "$DLBY_RUN_DIR/fuse-hub.sock" ]; do
	i=$((i + 1))
	[ "$i" -gt 40 ] && fail "hub socket did not appear within 2 s"
	sleep 0.05
done

"$B/tools/fake-sensors" radar 3 2>"$T/radar.log" &
radar=$!
"$B/tools/fake-sensors" sonar 1 2 2>"$T/sonar.log" &
sonar=$!
wait "$radar" || fail "radar sender exited $?"
radar=""
wait "$sonar" || fail "sonar sender exited $?"
sonar=""

sleep 0.2	# let the hub read the senders' EOFs
kill -TERM "$hub" || fail "hub already gone"
wait "$hub"
rc=$?
hub=""
[ "$rc" -eq 0 ] || fail "hub exited $rc"

grep -q -E 'AddressSanitizer|LeakSanitizer|runtime error' "$LOG" && fail "sanitizer report"
bad=$(grep -v -E '^fuse-hub\[[0-9]+\] [IWE] [a-z_]+( [a-z_]+=[^ ]*)*$' "$LOG")
[ -z "$bad" ] || fail "malformed log line: $bad"

up_r=$(line_of ' I sensor_up name=radar$')
up_s=$(line_of ' I sensor_up name=sonar$')
ok=$(line_of ' I healthy$')
deg=$(line_of ' W degraded lost=sonar reason=stale$')
blind=$(line_of ' E blind lost=radar,sonar reason=eof$')
[ -n "$up_r" ] || fail "no sensor_up name=radar"
[ -n "$up_s" ] || fail "no sensor_up name=sonar"
[ -n "$ok" ] || fail "no healthy"
[ -n "$deg" ] || fail "no degraded lost=sonar reason=stale"
[ -n "$blind" ] || fail "no blind lost=radar,sonar reason=eof"
[ "$up_r" -lt "$ok" ] && [ "$up_s" -lt "$ok" ] && [ "$ok" -lt "$deg" ] && [ "$deg" -lt "$blind" ] ||
	fail "lines out of order"

nwe=$(grep -c -E '^fuse-hub\[[0-9]+\] [WE] ' "$LOG")
[ "$nwe" -eq 2 ] || fail "expected exactly 2 W/E lines, got $nwe"

ex=$(grep -E ' I exit code=0 radar=[0-9]+ sonar=[0-9]+ rejected=0$' "$LOG") || fail "no clean exit line"
n=$(echo "$ex" | sed -E 's/.* radar=([0-9]+) .*/\1/')
m=$(echo "$ex" | sed -E 's/.* sonar=([0-9]+) .*/\1/')
[ "$n" -ge 27 ] && [ "$n" -le 32 ] || fail "radar count $n not in 27-32"
[ "$m" -ge 15 ] && [ "$m" -le 18 ] || fail "sonar count $m not in 15-18"

echo "hub_e2e: ok (radar=$n sonar=$m)"
