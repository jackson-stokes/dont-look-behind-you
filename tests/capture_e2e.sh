#!/bin/sh
# End-to-end for the capture processes without hardware. Usage: capture_e2e.sh <builddir>
#
# fake-ld2451 (pty) -> real cap-radar -> fuse-hub -> store for ~2 s, with junk,
# bad tails, bad lengths, a payload frame and a split frame on the wire; then
# SIGTERM cap-radar (exit 0) and verify the log with dlby-logcat. Also: cap-radar
# and cap-sonar exit 78 when their device is missing.
set -u
B=$1
T=$(mktemp -d /tmp/dlby-capture-e2e-XXXXXX) || exit 1
export DLBY_RUN_DIR="$T/run"
export DLBY_LOG_DIR="$T/log"
hub=""
store=""
fake=""
radar=""

cleanup() {
	for p in $radar $fake $hub $store; do
		kill -KILL "$p" 2>/dev/null
	done
	rm -rf "$T"
}
trap cleanup EXIT

fail() {
	echo "capture_e2e: FAIL: $*"
	for f in "$T"/*.log; do
		echo "---- $f"
		cat "$f"
	done
	exit 1
}

# field <name> <line>: value of name=... in a key=value line.
field() {
	echo "$2" | tr ' ' '\n' | sed -n "s/^$1=//p"
}

# Missing devices are configuration errors: 78, no restart.
DLBY_RADAR_DEV="$T/no-such-tty" "$B/cap-radar" 2>"$T/radar-missing.log"
rc=$?
[ "$rc" -eq 78 ] || fail "cap-radar with a missing device exited $rc, want 78"
grep -q -E '^cap-radar\[[0-9]+\] E open_failed dev=[^ ]+ err=' "$T/radar-missing.log" ||
	fail "no open_failed line from cap-radar"
DLBY_SONAR_CHIP="$T/no-such-chip" "$B/cap-sonar" 2>"$T/sonar-missing.log"
rc=$?
[ "$rc" -eq 78 ] || fail "cap-sonar with a missing chip exited $rc, want 78"
grep -q -E '^cap-sonar\[[0-9]+\] E open_failed chip=[^ ]+ err=' "$T/sonar-missing.log" ||
	fail "no open_failed line from cap-sonar"
DLBY_SONAR_TRIG=24 "$B/cap-sonar" 2>"$T/sonar-config.log"
rc=$?
[ "$rc" -eq 78 ] || fail "cap-sonar with TRIG == ECHO exited $rc, want 78"

"$B/store" 2>"$T/store.log" &
store=$!
i=0
until grep -q ' I start ' "$T/store.log" 2>/dev/null; do
	i=$((i + 1))
	[ "$i" -gt 40 ] && fail "store did not start within 2 s"
	sleep 0.05
done
"$B/fuse-hub" 2>"$T/hub.log" &
hub=$!
i=0
while [ ! -S "$DLBY_RUN_DIR/fuse-hub.sock" ]; do
	i=$((i + 1))
	[ "$i" -gt 40 ] && fail "hub socket did not appear within 2 s"
	sleep 0.05
done

"$B/tools/fake-ld2451" "$T/ttyRADAR" 3 2>"$T/fake.log" &
fake=$!
i=0
while [ ! -L "$T/ttyRADAR" ]; do
	i=$((i + 1))
	[ "$i" -gt 40 ] && fail "fake radar pty did not appear within 2 s"
	sleep 0.05
done

DLBY_RADAR_DEV="$T/ttyRADAR" "$B/cap-radar" 2>"$T/radar.log" &
radar=$!
sleep 2
kill -TERM "$radar" || fail "cap-radar already gone"
wait "$radar"
rc=$?
radar=""
[ "$rc" -eq 0 ] || fail "cap-radar exited $rc on SIGTERM, want 0"
wait "$fake" || fail "fake-ld2451 exited $?"
fake=""

sleep 0.2	# let the hub read cap-radar's EOF
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
bad=$(grep -v -E '^cap-radar\[[0-9]+\] [IWE] [a-z_]+( [a-z_]+=[^ ]*)*$' "$T/radar.log")
[ -z "$bad" ] || fail "malformed log line: $bad"

grep -q -E '^cap-radar\[[0-9]+\] I radar_up$' "$T/radar.log" || fail "no radar_up"
grep -q -E '^cap-radar\[[0-9]+\] W bad_frame reason=tail count=1$' "$T/radar.log" ||
	fail "no bad_frame reason=tail"
grep -q -E '^cap-radar\[[0-9]+\] W bad_frame reason=len count=1$' "$T/radar.log" ||
	fail "no bad_frame reason=len"
grep -q -E '^cap-radar\[[0-9]+\] I target_frame len=4 hex=f4f3f2f1040001020304f8f7f6f5 count=1$' \
	"$T/radar.log" || fail "no target_frame hex line"
grep -q -E '^cap-radar\[[0-9]+\] W radar_silent' "$T/radar.log" && fail "radar_silent while frames flowed"
grep -q -E '^fuse-hub\[[0-9]+\] I sensor_up name=radar$' "$T/hub.log" || fail "hub never saw the radar"

ex=$(grep -E '^cap-radar\[[0-9]+\] I exit code=0 ' "$T/radar.log")
[ -n "$ex" ] || fail "no clean exit line"
frames=$(field frames "$ex")
sent=$(field sent "$ex")
[ "$frames" -ge 8 ] || fail "too few frames: $ex"
[ "$(field target_frames "$ex")" -ge 1 ] || fail "no target frames counted: $ex"
[ "$(field bad_tail "$ex")" -ge 1 ] || fail "no bad tails counted: $ex"
[ "$(field bad_len "$ex")" -ge 1 ] || fail "no bad lengths counted: $ex"
[ "$sent" -eq "$frames" ] || fail "sent != frames: $ex"

sum=$("$B/tools/dlby-logcat" "$DLBY_LOG_DIR") || fail "dlby-logcat: $sum"
[ "$(field bad "$sum")" -eq 0 ] || fail "bad bytes kept: $sum"
[ "$(field contiguous "$sum")" -eq 1 ] || fail "seq not contiguous: $sum"
[ "$(field radar "$sum")" -eq "$sent" ] || fail "store has $(field radar "$sum") radar records, cap-radar sent $sent"

echo "capture_e2e: ok (frames=$frames sent=$sent $sum)"
