#!/bin/sh
# End-to-end: fake senders -> fuse-hub -> store, with store killed mid-stream and
# a torn write simulated before it restarts (DESIGN.md §3 rows store/power, §6 D).
# Usage: store_e2e.sh <builddir>
#
#   0.0 s  store, hub, radar + sonar senders for 5 s
#   1.5 s  kill -KILL store; append 5 garbage bytes to the newest segment
#   2.0 s  restart store -> torn_tail truncated_bytes=5; hub -> store_back
#   5.0 s  senders exit; TERM hub and store; verify the log with dlby-logcat
set -u
B=$1
T=$(mktemp -d /tmp/dlby-store-e2e-XXXXXX) || exit 1
export DLBY_RUN_DIR="$T/run"
export DLBY_LOG_DIR="$T/log"
hub=""
store=""
radar=""
sonar=""

cleanup() {
	for p in $hub $store $radar $sonar; do
		kill -KILL "$p" 2>/dev/null
	done
	rm -rf "$T"
}
trap cleanup EXIT

fail() {
	echo "store_e2e: FAIL: $*"
	for f in "$T"/*.log; do
		echo "---- $f"
		cat "$f"
	done
	exit 1
}

# start_store <n>: logs to store<n>.log; waits for its start line.
start_store() {
	"$B/store" 2>"$T/store$1.log" &
	store=$!
	i=0
	until grep -q ' I start ' "$T/store$1.log" 2>/dev/null; do
		i=$((i + 1))
		[ "$i" -gt 40 ] && fail "store $1 did not start within 2 s"
		sleep 0.05
	done
}

# field <name> <line>: value of name=... in a key=value line.
field() {
	echo "$2" | tr ' ' '\n' | sed -n "s/^$1=//p"
}

start_store 1
"$B/fuse-hub" 2>"$T/hub.log" &
hub=$!
i=0
while [ ! -S "$DLBY_RUN_DIR/fuse-hub.sock" ]; do
	i=$((i + 1))
	[ "$i" -gt 40 ] && fail "hub socket did not appear within 2 s"
	sleep 0.05
done

"$B/tools/fake-sensors" radar 5 2>"$T/radar.log" &
radar=$!
"$B/tools/fake-sensors" sonar 5 2>"$T/sonar.log" &
sonar=$!

sleep 1.5
kill -KILL "$store"
wait "$store" 2>/dev/null
store=""
seg=$(ls "$DLBY_LOG_DIR"/seg-*.log | tail -n 1)
printf 'XXXXX' >>"$seg"
sleep 0.5
start_store 2

wait "$radar" || fail "radar sender exited $?"
radar=""
wait "$sonar" || fail "sonar sender exited $?"
sonar=""
sleep 0.3
kill -TERM "$hub" || fail "hub already gone"
wait "$hub"
rc=$?
hub=""
[ "$rc" -eq 0 ] || fail "hub exited $rc"
kill -TERM "$store" || fail "store already gone"
wait "$store"
rc=$?
store=""
[ "$rc" -eq 0 ] || fail "store exited $rc"

cat "$T"/*.log | grep -q -E 'AddressSanitizer|LeakSanitizer|runtime error' && fail "sanitizer report"

grep -q -E '^store\[[0-9]+\] W torn_tail truncated_bytes=5$' "$T/store2.log" ||
	fail "no torn_tail truncated_bytes=5"
back=$(grep -E '^fuse-hub\[[0-9]+\] I store_back dropped=[0-9]+$' "$T/hub.log" | head -n 1)
[ -n "$back" ] || fail "no store_back"
[ "$(field dropped "$back")" -gt 0 ] || fail "store_back dropped=0"

ex=$(grep -E '^store\[[0-9]+\] I exit code=0 ' "$T/store2.log")
[ -n "$ex" ] || fail "store 2 has no clean exit line"
timed=$(field syncs_timed "$ex")
[ "$timed" -ge 2 ] || fail "store 2 syncs_timed=$timed, want >= 2 under a steady stream"

sum=$("$B/tools/dlby-logcat" "$DLBY_LOG_DIR") || fail "dlby-logcat: $sum"
[ "$(field bad "$sum")" -eq 0 ] || fail "bad bytes kept: $sum"
[ "$(field contiguous "$sum")" -eq 1 ] || fail "seq not contiguous: $sum"
[ "$(field store_gap "$sum")" -eq 2 ] || fail "want 2 store GAP records: $sum"
[ "$(field gap_msg "$sum")" -ge 1 ] || fail "no hub GAP message: $sum"
[ "$(field gap_dropped "$sum")" -gt 0 ] || fail "hub GAP dropped=0: $sum"
[ "$(field alert "$sum")" -ge 2 ] || fail "want >= 2 ALERT records: $sum"
[ "$(field radar "$sum")" -ge 25 ] || fail "too few radar records: $sum"
[ "$(field sonar "$sum")" -ge 40 ] || fail "too few sonar records: $sum"

echo "store_e2e: ok ($sum syncs_timed=$timed)"
