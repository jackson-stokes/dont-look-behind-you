#!/bin/sh
# End-to-end for the soak report (report.h, DESIGN.md §6 48 h soak). Usage: report_e2e.sh <builddir>
#
# dlby-super with the real store and fuse-hub and fake-sensors as the three cap-*,
# reporting every second:
#   start line with wall time, version and period; periodic system and per-process
#   lines (super itself and every child: up=1, rss_kb > 0, fds > 0); hub and store
#   counts; a killed cap-sonar shows restarts=1 in the next report; on TERM a
#   final=1 report of every process before any child stops, final counts from hub
#   and store before their exit lines, report_end, and exit code=0 last.
# Then DLBY_REPORT_S outside 1..3600 exits 78 in super, hub and store.
set -u
B=$(cd "$1" && pwd) || exit 1
T=$(mktemp -d /tmp/dlby-report-e2e-XXXXXX) || exit 1
export DLBY_RUN_DIR="$T/run" DLBY_LOG_DIR="$T/log" DLBY_BIN_DIR="$T/bin"
export DLBY_CHILDREN=store,fuse-hub,cap-radar,cap-radar-left,cap-sonar DLBY_REPORT_S=1
LOG="$T/super.log"
KIDS="store fuse-hub cap-radar cap-radar-left cap-sonar"
super=""

cleanup() {
	[ -n "$super" ] && kill -KILL "$super" 2>/dev/null
	for p in $(sed -n 's/^dlby-super\[[0-9]*\] I child_start name=[^ ]* pid=//p' "$LOG" 2>/dev/null); do
		kill -KILL "$p" 2>/dev/null
	done
	rm -rf "$T"
}
trap cleanup EXIT

fail() {
	echo "report_e2e: FAIL: $*"
	[ -f "$LOG" ] && { echo "---- $LOG"; cat "$LOG"; }
	exit 1
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

field() {
	echo "$2" | tr ' ' '\n' | sed -n "s/^$1=//p"
}

# The line of report n for process name.
child_line() {
	grep -E "^dlby-super\[[0-9]+\] I report_child n=$1 name=$2 " "$LOG"
}

mkdir -p "$T/bin" || fail "mkdir"
ln -s "$B/store" "$T/bin/store" && ln -s "$B/fuse-hub" "$T/bin/fuse-hub" || fail "symlinks"
for n in cap-radar cap-radar-left cap-sonar; do
	ln -s "$B/tools/fake-sensors" "$T/bin/$n" || fail "symlink $n"
done

"$B/dlby-super" 2>"$LOG" &
super=$!
for n in $KIDS; do
	wait_count "^dlby-super\[[0-9]+\] I first_heartbeat name=$n " "$LOG" 1 3000 ||
		fail "no first_heartbeat for $n"
done

grep -q -E '^dlby-super\[[0-9]+\] I start wall=[0-9]{4}-[0-9]{2}-[0-9]{2}T[0-9]{2}:[0-9]{2}:[0-9]{2}Z version=[^ ]+ report_s=1 ' "$LOG" ||
	fail "start line lacks wall=, version= or report_s=1"
grep -q -E '^dlby-super\[[0-9]+\] I report n=0 .*cpu_pct=-1\.0 ' "$LOG" ||
	fail "no baseline report n=0"

# Report 2: every process up, with real numbers. Wait for its last child line.
wait_count '^dlby-super\[[0-9]+\] I report_child n=2 name=cap-sonar ' "$LOG" 1 4000 ||
	fail "no report 2"
r=$(grep -E '^dlby-super\[[0-9]+\] I report n=2 ' "$LOG")
for k in uptime_s cpu_pct load1 mem_avail_kb temp_mc; do
	v=$(field "$k" "$r")
	[ -n "$v" ] || fail "report 2 lacks $k: $r"
done
cpu=$(field cpu_pct "$r")
case "$cpu" in -*) fail "report 2 cpu_pct not measured: $r" ;; esac
echo "$r" | grep -q ' err=' && fail "report 2 has a read error: $r"
for n in dlby-super $KIDS; do
	l=$(child_line 2 "$n")
	[ -n "$l" ] || fail "report 2 has no line for $n"
	[ "$(field up "$l")" = 1 ] || fail "$n not up in report 2: $l"
	[ "$(field rss_kb "$l")" -gt 0 ] 2>/dev/null || fail "$n rss_kb not > 0: $l"
	[ "$(field fds "$l")" -gt 0 ] 2>/dev/null || fail "$n fds not > 0: $l"
	[ "$(field restarts "$l")" = 0 ] || fail "$n restarted before any fault: $l"
	echo "$l" | grep -q ' err=' && fail "read error: $l"
done

wait_count '^fuse-hub\[[0-9]+\] I counts n=2 ' "$LOG" 1 3000 || fail "no hub counts n=2"
wait_count '^store\[[0-9]+\] I counts n=2 ' "$LOG" 1 3000 || fail "no store counts n=2"
h=$(grep -E '^fuse-hub\[[0-9]+\] I counts n=2 ' "$LOG")
[ "$(field sonar "$h")" -gt 0 ] && [ "$(field health "$h")" = OK ] || fail "hub counts: $h"
s=$(grep -E '^store\[[0-9]+\] I counts n=2 ' "$LOG")
[ "$(field records "$s")" -gt 0 ] || fail "store counts: $s"

# A killed cap-sonar shows as one restart in the next report.
sonar=$(sed -n 's/^dlby-super\[[0-9]*\] I child_start name=cap-sonar pid=//p' "$LOG" | tail -n 1)
kill -KILL "$sonar" || fail "kill cap-sonar"
wait_count '^dlby-super\[[0-9]+\] I first_heartbeat name=cap-sonar ' "$LOG" 2 3000 ||
	fail "cap-sonar not restarted"
last=$(sed -n 's/^dlby-super\[[0-9]*\] I report n=\([0-9]*\) .*/\1/p' "$LOG" | tail -n 1)
next=$((last + 1))
wait_count "^dlby-super\[[0-9]+\] I report_child n=$next name=cap-sonar " "$LOG" 1 3000 ||
	fail "no report after the restart"
l=$(child_line "$next" cap-sonar)
[ "$(field restarts "$l")" = 1 ] && [ "$(field up "$l")" = 1 ] || fail "after restart: $l"
[ "$(field pid "$l")" != "$sonar" ] || fail "report shows the dead pid: $l"
l=$(child_line "$next" fuse-hub)
[ "$(field restarts "$l")" = 0 ] || fail "fuse-hub restart counted: $l"

kill -TERM "$super" || fail "super already gone"
wait "$super"
rc=$?
super=""
[ "$rc" -eq 0 ] || fail "dlby-super exited $rc"

# Final report: right after the stop line, every process, before any child stops.
tail -n +"$(grep -n -E '^dlby-super\[[0-9]+\] I stop signal=TERM$' "$LOG" | cut -d: -f1)" "$LOG" \
	> "$T/after"
fin=$(sed -n 's/^dlby-super\[[0-9]*\] I report n=\([0-9]*\) .* final=1.*/\1/p' "$T/after")
[ -n "$fin" ] || fail "no final report after stop"
exit_at=$(grep -n -E '^dlby-super\[[0-9]+\] [IE] child_exit ' "$T/after" | head -n 1 | cut -d: -f1)
for n in dlby-super $KIDS; do
	at=$(grep -n -E "^dlby-super\[[0-9]+\] I report_child n=$fin name=$n .* final=1" "$T/after" |
		cut -d: -f1)
	[ -n "$at" ] || fail "final report lacks $n"
	[ "$at" -lt "$exit_at" ] || fail "final line for $n after a child stopped"
done
for p in fuse-hub store; do
	c=$(grep -n -E "^$p\[[0-9]+\] I counts .* final=1$" "$LOG" | tail -n 1 | cut -d: -f1)
	e=$(grep -n -E "^$p\[[0-9]+\] [IE] exit code=" "$LOG" | tail -n 1 | cut -d: -f1)
	[ -n "$c" ] && [ -n "$e" ] && [ "$c" -lt "$e" ] || fail "$p final counts not before its exit"
done
s=$(grep -E '^store\[[0-9]+\] I counts .* final=1$' "$LOG" | tail -n 1)
e=$(grep -E '^store\[[0-9]+\] I exit code=0 ' "$LOG" | tail -n 1)
[ "$(field records "$s")" = "$(field records "$e")" ] || fail "store final counts != exit: $s / $e"
end=$(grep -E '^dlby-super\[[0-9]+\] I report_end ' "$LOG")
[ "$(field restarts "$end")" = 1 ] && [ "$(field reports "$end")" = "$fin" ] ||
	fail "report_end: $end (final report $fin)"
tail -n 2 "$LOG" | head -n 1 | grep -q -E '^dlby-super\[[0-9]+\] I report_end ' ||
	fail "report_end is not just before the exit line"
tail -n 1 "$LOG" | grep -q -E '^dlby-super\[[0-9]+\] I exit code=0$' || fail "exit line not last"

# Bad periods exit 78 (EX_CONFIG), so super disables rather than restarts.
for v in 0 3601 abc; do
	DLBY_REPORT_S=$v "$B/dlby-super" 2>"$T/bad.log"
	[ $? -eq 78 ] || fail "super DLBY_REPORT_S=$v: want 78"
	grep -q 'bad_config DLBY_REPORT_S=' "$T/bad.log" || fail "super: no bad_config line"
done
for p in fuse-hub store; do
	DLBY_REPORT_S=7200 DLBY_RUN_DIR="$T/run-$p" DLBY_LOG_DIR="$T/log-$p" "$B/$p" 2>"$T/bad.log"
	[ $? -eq 78 ] || fail "$p DLBY_REPORT_S=7200: want 78"
	grep -q 'bad_config DLBY_REPORT_S=7200' "$T/bad.log" || fail "$p: no bad_config line"
done

echo "report_e2e: ok (reports=$fin $(echo "$r" | sed 's/^.* I report //'))"
