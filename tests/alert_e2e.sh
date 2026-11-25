#!/bin/sh
# End-to-end for alert (DESIGN.md §1 patterns, §2 E). Usage: alert_e2e.sh <builddir>
#
# alert with no GPIO ($DLBY_ALERT_CHIP=none) and a trace file standing in for the
# LEDs and buzzer, fed by tools/send-alert in place of fuse-hub:
#   starts in HUB_LOST with every output on (its on-phase); per-side pattern lines
#   in order; while the right side is IN_ZONE and the left APPROACHING, both LEDs
#   stay solid and the buzzer beeps at 4 Hz; 1 s of silence -> hub_lost
#   (silent_ms 1000..1500) and both sides HUB_LOST; messages again -> hub_back; an
#   invalid state is rejected and counted; SIGTERM -> final counts, exit 0, and the
#   trace ends with everything off.
# Then bad settings exit 78, and dlby-super --outputs-off works with and without GPIO.
set -u
B=$(cd "$1" && pwd) || exit 1
T=$(mktemp -d /tmp/dlby-alert-e2e-XXXXXX) || exit 1
export DLBY_RUN_DIR="$T/run" DLBY_ALERT_CHIP=none DLBY_ALERT_TRACE="$T/trace" DLBY_REPORT_S=1
LOG="$T/alert.log"
TR="$T/trace"
alert=""

cleanup() {
	[ -n "$alert" ] && kill -KILL "$alert" 2>/dev/null
	rm -rf "$T"
}
trap cleanup EXIT

fail() {
	echo "alert_e2e: FAIL: $*"
	for f in "$LOG" "$T/send1.log" "$T/send2.log"; do
		[ -f "$f" ] && { echo "---- $f"; cat "$f"; }
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

field() {
	echo "$2" | tr ' ' '\n' | sed -n "s/^$1=//p"
}

# t_ms of the first "pattern side=<side> state=<state>" line.
t_of() {
	field t_ms "$(grep -E "^alert\[[0-9]+\] I pattern side=$1 state=$2 " "$LOG" | head -n 1)"
}

"$B/alert" 2>"$LOG" &
alert=$!
wait_for '^alert\[[0-9]+\] I start sock=.* gpio=none trace=' "$LOG" 3000 || fail "no start line"
wait_for '.' "$TR" 1000 || fail "no trace"
# HUB_LOST until the hub speaks: both LEDs blink and the chirp start, all on at once.
[ "$(head -n 1 "$TR" | cut -d' ' -f2-)" = "1 1 1" ] || fail "first trace line: $(head -n 1 "$TR")"

"$B/tools/send-alert" CLEAR:CLEAR:600 IN_ZONE:APPROACHING:1200 hold:1700 2>"$T/send1.log" ||
	fail "send-alert 1"
wait_for '^alert\[[0-9]+\] W hub_lost ' "$LOG" 1000 || fail "no hub_lost"
"$B/tools/send-alert" CLEAR:BLIND:600 bad:100 CLEAR:CLEAR:300 2>"$T/send2.log" ||
	fail "send-alert 2"
wait_for '^alert\[[0-9]+\] I hub_back$' "$LOG" 1000 || fail "no hub_back"
wait_for '^alert\[[0-9]+\] I counts n=2 ' "$LOG" 3000 || fail "no periodic counts"

kill -TERM "$alert" || fail "alert already gone"
wait "$alert"
rc=$?
alert=""
[ "$rc" -eq 0 ] || fail "alert exited $rc"

# Each side's pattern changes, in order.
seq=$(sed -n 's/^alert\[[0-9]*\] I pattern side=\([a-z]*\) state=\([A-Z_]*\) .*/\1:\2/p' "$LOG" |
	tr '\n' ' ')
want="right:CLEAR left:CLEAR right:IN_ZONE left:APPROACHING right:HUB_LOST left:HUB_LOST right:CLEAR left:BLIND left:CLEAR "
[ "$seq" = "$want" ] || fail "pattern sequence: $seq"
[ "$(grep -c -E '^alert\[[0-9]+\] I hub_up$' "$LOG")" -eq 1 ] || fail "hub_up not once"

# hub_lost: 1 s of silence, caught within half a second, before the HUB_LOST lines.
l=$(grep -E '^alert\[[0-9]+\] W hub_lost ' "$LOG")
ms=$(field silent_ms "$l")
[ "$ms" -ge 1000 ] && [ "$ms" -le 1500 ] || fail "hub_lost after $ms ms: $l"
at_lost=$(grep -n -E '^alert\[[0-9]+\] W hub_lost ' "$LOG" | cut -d: -f1)
at_pat=$(grep -n -E '^alert\[[0-9]+\] I pattern side=right state=HUB_LOST ' "$LOG" | cut -d: -f1)
[ "$at_lost" -lt "$at_pat" ] || fail "HUB_LOST pattern before the hub_lost line"

# While right=IN_ZONE and left=APPROACHING: both LEDs solid, buzzer at 4 Hz.
t_in=$(t_of right IN_ZONE)
t_out=$(t_of right HUB_LOST)
[ -n "$t_in" ] && [ -n "$t_out" ] || fail "no IN_ZONE window"
win=$((t_out - t_in))
awk -v a="$t_in" -v b="$t_out" '$1 >= a && $1 < b' "$TR" > "$T/win"
[ -s "$T/win" ] || fail "no trace lines in the IN_ZONE window"
awk '$2 != 1 || $3 != 1' "$T/win" | grep -q . && fail "an LED went off during IN_ZONE/APPROACHING: $(cat "$T/win")"
rises=$(awk 'BEGIN { p = 0 } { if ($4 == 1 && p == 0) n++; p = $4 } END { print n + 0 }' "$T/win")
want_rises=$(((win + 249) / 250))
[ "$rises" -ge $((want_rises - 1)) ] && [ "$rises" -le $((want_rises + 1)) ] ||
	fail "buzzer rose $rises times in $win ms (want $want_rises +-1 at 4 Hz)"

# The invalid state: rejected and counted; final counts; outputs off at the end.
grep -q -E '^alert\[[0-9]+\] W bad_msg reason=state state=99 ' "$LOG" || fail "bad state not logged"
c=$(grep -E '^alert\[[0-9]+\] I counts .* final=1$' "$LOG")
[ -n "$c" ] || fail "no final counts"
[ "$(field rejected "$c")" = 1 ] && [ "$(field hub_lost "$c")" = 1 ] &&
	[ "$(field msgs "$c")" -gt 5 ] && [ "$(field write_errs "$c")" = 0 ] || fail "final counts: $c"
[ "$(field right "$c")" = CLEAR ] && [ "$(field left "$c")" = CLEAR ] || fail "final states: $c"
[ "$(tail -n 1 "$TR" | cut -d' ' -f2-)" = "0 0 0" ] || fail "outputs not off at exit: $(tail -n 1 "$TR")"
tail -n 1 "$LOG" | grep -q -E '^alert\[[0-9]+\] I exit code=0$' || fail "exit line not last"

# Bad settings: 78, so dlby-super disables alert rather than restarting it.
for kv in DLBY_ALERT_LED_LEFT=17 DLBY_ALERT_BUZZ=abc DLBY_REPORT_S=0; do
	env "$kv" "$B/alert" 2>"$T/bad.log"
	[ $? -eq 78 ] || fail "alert $kv: want 78"
	grep -q 'bad_config' "$T/bad.log" || fail "alert $kv: no bad_config line"
done
DLBY_ALERT_CHIP=/nonexistent/gpiochip0 "$B/alert" 2>"$T/bad.log"
[ $? -eq 78 ] || fail "alert with a missing chip: want 78"
grep -q 'gpio_open_failed chip=/nonexistent/gpiochip0 lines=17,27,22' "$T/bad.log" ||
	fail "missing chip: no gpio_open_failed line"

# dlby-super --outputs-off (systemd ExecStopPost).
"$B/dlby-super" --outputs-off 2>"$T/off.log" || fail "--outputs-off with none: exit $?"
grep -q -E '^dlby-super\[[0-9]+\] I outputs_off gpio=none$' "$T/off.log" || fail "no outputs_off line"
DLBY_ALERT_CHIP=/nonexistent/gpiochip0 "$B/dlby-super" --outputs-off 2>"$T/off.log"
[ $? -eq 78 ] || fail "--outputs-off with a missing chip: want 78"
grep -q 'outputs_off_failed' "$T/off.log" || fail "no outputs_off_failed line"
DLBY_ALERT_LED_RIGHT=22 "$B/dlby-super" --outputs-off 2>"$T/off.log"
[ $? -eq 78 ] || fail "--outputs-off with a bad setting: want 78"
"$B/dlby-super" --bogus 2>"$T/off.log"
[ $? -eq 64 ] || fail "dlby-super --bogus: want 64"

grep -q -E 'AddressSanitizer|LeakSanitizer|runtime error' "$LOG" && fail "sanitizer report"

echo "alert_e2e: ok (hub_lost_ms=$ms in_zone_ms=$win buzzer_rises=$rises $(echo "$c" | sed 's/^.* I counts //'))"
