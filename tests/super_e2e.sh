#!/bin/sh
# End-to-end for dlby-super (DESIGN.md §2 E, §3, §6 "E restart"). Usage: super_e2e.sh <builddir>
#
# Children: the real store and fuse-hub; fake-sensors as cap-sonar and cap-radar-left,
# so real sonar and left-radar data flows -> fuse-hub -> store; fake-child as alert
# (crashes once at 1 s) and cap-radar (hangs once at 1 s, ignoring SIGTERM).
#
# Phase 1, one super:
#   start order and first heartbeats; kill -SEGV fuse-hub -> new hub within 1 s and
#   sonar keeps reaching store; kill -KILL store -> restarted; alert crash -> its
#   outputs forced off, restart; cap-radar hang -> heartbeat_lost, KILL, restart;
#   inherited fds only 0-3; TERM super -> store stopped last, exit 0, no child left.
# Phase 2: kill -KILL super -> every child exits within 1 s (PDEATHSIG, hb EOF).
# Phase 3: a child exiting 78 (fake-child as cap-radar-left) -> disabled, never
#   restarted, inherited fds only 0-3; TERM super -> exit 0.
set -u
B=$(cd "$1" && pwd) || exit 1
T=$(mktemp -d /tmp/dlby-super-e2e-XXXXXX) || exit 1
export DLBY_RUN_DIR="$T/run" DLBY_LOG_DIR="$T/log" DLBY_BIN_DIR="$T/bin" FAKE_STATE_DIR="$T/state"
export FAKE_alert=crash_once:1 FAKE_cap_radar=hang_once:1
# No GPIO in the test: super logs the forced-off outputs as gpio=none.
export DLBY_ALERT_CHIP=none
# The SIGSEGVs here are intentional (fake alert, kill -SEGV on the hub): they must
# kill by signal as in production, not become an ASan report and exit 1. Only this
# test sets it; ASan's memory checks stay on.
export ASAN_OPTIONS="${ASAN_OPTIONS:+$ASAN_OPTIONS:}handle_segv=0"
LOG="$T/super1.log"
LOG2="$T/super2.log"
LOG3="$T/super3.log"
super=""

# Every child pid any super started.
all_pids() {
	cat "$LOG" "$LOG2" "$LOG3" 2>/dev/null |
		sed -n 's/^dlby-super\[[0-9]*\] I child_start name=[^ ]* pid=//p'
}

cleanup() {
	[ -n "$super" ] && kill -KILL "$super" 2>/dev/null
	for p in $(all_pids); do
		kill -KILL "$p" 2>/dev/null
	done
	rm -rf "$T"
}
trap cleanup EXIT

fail() {
	echo "super_e2e: FAIL: $*"
	for f in "$LOG" "$LOG2" "$LOG3"; do
		[ -f "$f" ] || continue
		echo "---- $f"
		cat "$f"
	done
	exit 1
}

# wait_for <regex> <file> <timeout_ms>
wait_for() {
	i=0
	while ! grep -q -E "$1" "$2" 2>/dev/null; do
		i=$((i + 1))
		[ "$i" -gt $(($3 / 20)) ] && return 1
		sleep 0.02
	done
}

count() {
	grep -c -E "$1" "$2" 2>/dev/null
}

# wait_count <regex> <file> <n> <timeout_ms>
wait_count() {
	i=0
	while [ "$(count "$1" "$2")" -lt "$3" ]; do
		i=$((i + 1))
		[ "$i" -gt $(($4 / 20)) ] && return 1
		sleep 0.02
	done
}

pid_of() {
	sed -n "s/^dlby-super\[[0-9]*\] I child_start name=$1 pid=//p" "$2" | tail -n 1
}

field() {
	echo "$2" | tr ' ' '\n' | sed -n "s/^$1=//p"
}

# Running and not a zombie (orphans may never be reaped inside a container).
alive() {
	[ -r "/proc/$1/stat" ] || return 1
	s=$(sed -n 's/^[0-9]* (.*) \(.\) .*/\1/p' "/proc/$1/stat" 2>/dev/null)
	[ -n "$s" ] && [ "$s" != Z ]
}

sonar_records() {
	field sonar "$("$B/tools/dlby-logcat" "$DLBY_LOG_DIR" 2>/dev/null)"
}

mkdir -p "$T/bin" "$T/state" || fail "mkdir"
ln -s "$B/store" "$T/bin/store" && ln -s "$B/fuse-hub" "$T/bin/fuse-hub" &&
	ln -s "$B/tools/fake-sensors" "$T/bin/cap-sonar" &&
	ln -s "$B/tools/fake-sensors" "$T/bin/cap-radar-left" || fail "symlinks"
for n in alert cap-radar; do
	ln -s "$B/tools/fake-child" "$T/bin/$n" || fail "symlink $n"
done

# ---- phase 1
"$B/dlby-super" 2>"$LOG" &
super=$!
for n in store fuse-hub alert cap-radar cap-radar-left cap-sonar; do
	wait_for "^dlby-super\[[0-9]+\] I first_heartbeat name=$n " "$LOG" 3000 ||
		fail "no first_heartbeat for $n"
done
order=$(sed -n 's/^dlby-super\[[0-9]*\] I child_start name=\([^ ]*\) .*/\1/p' "$LOG" | head -n 6 | tr '\n' ' ')
[ "$order" = "store fuse-hub alert cap-radar cap-radar-left cap-sonar " ] ||
	fail "start order: $order"

# fuse-hub crash: a new hub must be up within 1 s (§6 E restart).
hub=$(pid_of fuse-hub "$LOG")
t0=$(date +%s%N)
kill -SEGV "$hub" || fail "kill -SEGV fuse-hub"
wait_count '^fuse-hub\[[0-9]+\] I start ' "$LOG" 2 2000 || fail "fuse-hub did not restart"
t1=$(date +%s%N)
restart_ms=$(((t1 - t0) / 1000000))
[ "$restart_ms" -le 1000 ] || fail "fuse-hub restart took ${restart_ms} ms (> 1000)"
grep -q -E '^dlby-super\[[0-9]+\] E child_exit name=fuse-hub signal=SEGV$' "$LOG" ||
	fail "no child_exit name=fuse-hub signal=SEGV"
grep -q -E '^dlby-super\[[0-9]+\] I child_restart name=fuse-hub in_ms=100$' "$LOG" ||
	fail "no child_restart for fuse-hub"

# Sonar data keeps reaching store through the new hub.
s1=$(sonar_records)
sleep 1.5
s2=$(sonar_records)
[ -n "$s1" ] && [ -n "$s2" ] && [ $((s2 - s1)) -ge 15 ] ||
	fail "sonar records after hub restart: $s1 -> $s2 (want +15)"

# store killed outright: restarted.
st=$(pid_of store "$LOG")
kill -KILL "$st" || fail "kill -KILL store"
wait_count '^store\[[0-9]+\] I start ' "$LOG" 2 2000 || fail "store did not restart"
grep -q -E '^dlby-super\[[0-9]+\] E child_exit name=store signal=KILL$' "$LOG" ||
	fail "no child_exit name=store signal=KILL"

# alert crashed by itself at 1 s: restarted.
grep -q -E '^dlby-super\[[0-9]+\] E child_exit name=alert signal=SEGV$' "$LOG" ||
	fail "no child_exit name=alert signal=SEGV"
wait_count '^dlby-super\[[0-9]+\] I child_start name=alert ' "$LOG" 2 2000 || fail "alert not restarted"
# Its outputs were forced off between the crash and the restart (DESIGN.md §2 E).
crash_at=$(grep -n -E '^dlby-super\[[0-9]+\] E child_exit name=alert signal=SEGV$' "$LOG" | cut -d: -f1)
off_at=$(grep -n -E '^dlby-super\[[0-9]+\] I outputs_forced_off gpio=none$' "$LOG" | head -n 1 | cut -d: -f1)
restart_at=$(grep -n -E '^dlby-super\[[0-9]+\] I child_start name=alert ' "$LOG" | sed -n 2p | cut -d: -f1)
[ -n "$off_at" ] && [ "$off_at" -gt "$crash_at" ] && [ "$off_at" -lt "$restart_at" ] ||
	fail "outputs not forced off between alert's crash and restart"

# cap-radar hung at 1 s: heartbeat lost after 5 s, TERM ignored, KILL, restart.
wait_for '^dlby-super\[[0-9]+\] W child_kill name=cap-radar signal=KILL$' "$LOG" 9000 ||
	fail "cap-radar was not killed"
grep -q -E '^dlby-super\[[0-9]+\] E heartbeat_lost name=cap-radar$' "$LOG" ||
	fail "no heartbeat_lost name=cap-radar"
wait_count '^dlby-super\[[0-9]+\] I child_start name=cap-radar ' "$LOG" 2 2000 ||
	fail "cap-radar not restarted"
# The second first_heartbeat: the first run's line already matches a plain wait_for.
wait_count '^dlby-super\[[0-9]+\] I first_heartbeat name=cap-radar ' "$LOG" 2 3000 ||
	fail "restarted cap-radar never heartbeat"

kill -TERM "$super" || fail "super already gone"
wait "$super"
rc=$?
super=""
[ "$rc" -eq 0 ] || fail "dlby-super exited $rc"
grep -q -E '^dlby-super\[[0-9]+\] I exit code=0$' "$LOG" || fail "no clean exit line"
last=$(sed -n 's/^dlby-super\[[0-9]*\] [IE] child_exit name=\([^ ]*\) .*/\1/p' "$LOG" | tail -n 1)
[ "$last" = store ] || fail "store was not the last child to stop (last: $last)"
for p in $(all_pids); do
	alive "$p" && fail "child $p outlived dlby-super"
done

# Each fake child, first runs and restarts alike (alert x2, cap-radar x2), inherited
# only 0-3. The exit-78 child's line is checked in phase 3.
nfds=$(count '^(alert|cap-radar)\[[0-9]+\] I fds open=' "$LOG")
[ "$nfds" -ge 4 ] || fail "want >= 4 fds lines, got $nfds"
bad=$(grep -E '^(alert|cap-radar)\[[0-9]+\] I fds open=' "$LOG" | grep -v 'open=0,1,2,3$')
[ -z "$bad" ] || fail "leaked fds: $bad"

sum=$("$B/tools/dlby-logcat" "$DLBY_LOG_DIR") || fail "dlby-logcat: $sum"
[ "$(field bad "$sum")" -eq 0 ] && [ "$(field contiguous "$sum")" -eq 1 ] || fail "store log: $sum"

# ---- phase 2: super dies by SIGKILL; the children must follow.
"$B/dlby-super" 2>"$LOG2" &
super=$!
for n in store fuse-hub alert cap-radar cap-radar-left cap-sonar; do
	wait_for "^dlby-super\[[0-9]+\] I first_heartbeat name=$n " "$LOG2" 3000 ||
		fail "phase 2: no first_heartbeat for $n"
done
pids2=$(sed -n 's/^dlby-super\[[0-9]*\] I child_start name=[^ ]* pid=//p' "$LOG2")
kill -KILL "$super" || fail "kill -KILL super"
wait "$super" 2>/dev/null
super=""
i=0
for p in $pids2; do
	while alive "$p"; do
		i=$((i + 1))
		[ "$i" -gt 50 ] && fail "child $p outlived a killed dlby-super by > 1 s"
		sleep 0.02
	done
done

# ---- phase 3: a child that exits 78 (EX_CONFIG): logged once, never restarted.
# (Was status-lcd's job in phase 1 before status-lcd was cut.)
mkdir -p "$T/bin78" && ln -s "$B/tools/fake-child" "$T/bin78/cap-radar-left" ||
	fail "phase 3: symlink"
DLBY_BIN_DIR="$T/bin78" DLBY_CHILDREN=cap-radar-left FAKE_cap_radar_left=exit:78 \
	"$B/dlby-super" 2>"$LOG3" &
super=$!
wait_for '^dlby-super\[[0-9]+\] E child_disabled name=cap-radar-left code=78$' "$LOG3" 3000 ||
	fail "phase 3: no child_disabled for cap-radar-left"
grep -q -E '^dlby-super\[[0-9]+\] E child_exit name=cap-radar-left code=78$' "$LOG3" ||
	fail "phase 3: no child_exit name=cap-radar-left code=78"
# Ten times the first restart backoff (100 ms): a restart would have happened by now.
sleep 1
[ "$(count '^dlby-super\[[0-9]+\] I child_start name=cap-radar-left ' "$LOG3")" -eq 1 ] ||
	fail "phase 3: cap-radar-left was restarted"
grep -q -E '^cap-radar-left\[[0-9]+\] I fds open=0,1,2,3$' "$LOG3" ||
	fail "phase 3: exit-78 child inherited more than 0-3"
kill -TERM "$super" || fail "phase 3: super already gone"
wait "$super"
rc=$?
super=""
[ "$rc" -eq 0 ] || fail "phase 3: dlby-super exited $rc"
grep -q -E '^dlby-super\[[0-9]+\] I exit code=0$' "$LOG3" || fail "phase 3: no clean exit line"

cat "$LOG" "$LOG2" "$LOG3" | grep -q -E 'AddressSanitizer|LeakSanitizer|runtime error' &&
	fail "sanitizer report"

echo "super_e2e: ok (hub_restart_ms=$restart_ms sonar=$s1->$s2 $sum)"
