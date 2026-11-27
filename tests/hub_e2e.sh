#!/bin/sh
# End-to-end: fuse-hub plus two fake senders at the real rates; checks the hub's
# DESIGN.md §3 log lines and exit code. Usage: hub_e2e.sh <builddir>
#
# Phase 1, health (no alert running):
#   radar sends for 3 s, then exits              -> EOF at ~3 s
#   left radar sends for 2 s, then exits         -> EOF at ~2 s
#   sonar sends for 1 s, then holds silent 2 s   -> stale at ~1.44 s
# Phase 2, fusion -> alert: a real alert (no GPIO) and three senders in pass mode
#   (a 20 km/h cyclist from 30 m every 8 s, on both sides at once):
#   right APPROACHING -> IN_ZONE -> CLEAR, left APPROACHING -> CLEAR, at the hub
#   and at alert; no hub_lost while the hub runs (2 Hz refresh); sonar killed in
#   the second right approach -> DEG_APPROACHING; hub stopped -> alert hub_lost
#   within 1.0-1.5 s.
# Phase 3: DLBY_FUSE_RADAR=presence starts in presence mode; bad fusion settings
#   exit 78.
set -u
B=$1
T=$(mktemp -d /tmp/dlby-e2e-XXXXXX) || exit 1
export DLBY_RUN_DIR="$T/run"
LOG="$T/hub.log"
LOG2="$T/hub2.log"
ALOG="$T/alert.log"
hub=""
radar=""
left=""
sonar=""
alert=""

cleanup() {
	for p in $hub $radar $left $sonar $alert; do
		kill -KILL "$p" 2>/dev/null
	done
	rm -rf "$T"
}
trap cleanup EXIT

fail() {
	echo "hub_e2e: FAIL: $*"
	for f in "$LOG" "$T/radar.log" "$T/left.log" "$T/sonar.log" "$LOG2" "$ALOG"; do
		echo "---- $f"
		cat "$f" 2>/dev/null
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

# The "to" states of one side's state lines in the hub log, space-separated.
states() {
	sed -n "s/^fuse-hub\[[0-9]*\] I state side=$1 from=[A-Z_]* to=\([A-Z_]*\)$/\1/p" "$2" |
		tr '\n' ' '
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
"$B/tools/fake-sensors" radar_left 2 2>"$T/left.log" &
left=$!
"$B/tools/fake-sensors" sonar 1 2 2>"$T/sonar.log" &
sonar=$!
wait "$radar" || fail "radar sender exited $?"
radar=""
wait "$left" || fail "left radar sender exited $?"
left=""
wait "$sonar" || fail "sonar sender exited $?"
sonar=""

sleep 0.2	# let the hub read the senders' EOFs
kill -TERM "$hub" || fail "hub already gone"
wait "$hub"
rc=$?
hub=""
[ "$rc" -eq 0 ] || fail "hub exited $rc"

grep -q -E 'AddressSanitizer|LeakSanitizer|runtime error' "$LOG" && fail "sanitizer report"
# Keys start with a letter and may hold digits (p99_us); values have no spaces.
bad=$(grep -v -E '^fuse-hub\[[0-9]+\] [IWE] [a-z_]+( [a-z][a-z0-9_]*=[^ ]*)*$' "$LOG")
[ -z "$bad" ] || fail "malformed log line: $bad"

up_r=$(line_of ' I sensor_up name=radar$')
up_l=$(line_of ' I sensor_up name=radar_left$')
up_s=$(line_of ' I sensor_up name=sonar$')
ok=$(line_of ' I healthy$')
deg=$(line_of ' W degraded lost=sonar reason=stale$')
deg_l=$(line_of ' W degraded lost=radar_left reason=eof$')
blind=$(line_of ' E blind lost=radar,sonar,radar_left reason=eof$')
[ -n "$up_r" ] || fail "no sensor_up name=radar"
[ -n "$up_l" ] || fail "no sensor_up name=radar_left"
[ -n "$up_s" ] || fail "no sensor_up name=sonar"
[ -n "$ok" ] || fail "no healthy"
[ -n "$deg" ] || fail "no degraded lost=sonar reason=stale"
[ -n "$deg_l" ] || fail "no degraded lost=radar_left reason=eof"
[ -n "$blind" ] || fail "no blind lost=radar,sonar,radar_left reason=eof"
[ "$up_r" -lt "$ok" ] && [ "$up_l" -lt "$ok" ] && [ "$up_s" -lt "$ok" ] && [ "$ok" -lt "$deg" ] &&
	[ "$deg" -lt "$deg_l" ] && [ "$deg_l" -lt "$blind" ] || fail "lines out of order"

nwe=$(grep -c -E '^fuse-hub\[[0-9]+\] [WE] ' "$LOG")
[ "$nwe" -eq 3 ] || fail "expected exactly 3 W/E lines, got $nwe"

ex=$(grep -E ' I exit code=0 radar=[0-9]+ radar_left=[0-9]+ sonar=[0-9]+ rejected=0$' "$LOG") ||
	fail "no clean exit line"
n=$(echo "$ex" | sed -E 's/.* radar=([0-9]+) .*/\1/')
l=$(echo "$ex" | sed -E 's/.* radar_left=([0-9]+) .*/\1/')
m=$(echo "$ex" | sed -E 's/.* sonar=([0-9]+) .*/\1/')
[ "$n" -ge 27 ] && [ "$n" -le 32 ] || fail "radar count $n not in 27-32"
[ "$l" -ge 17 ] && [ "$l" -le 22 ] || fail "radar_left count $l not in 17-22"
[ "$m" -ge 15 ] && [ "$m" -le 18 ] || fail "sonar count $m not in 15-18"

# ---- phase 2: fusion -> alert, cyclist passes.
LAT="$T/lat"
mkdir -p "$LAT" || fail "phase 2: mkdir"
DLBY_ALERT_CHIP=none DLBY_LAT_DIR="$LAT" "$B/alert" 2>"$ALOG" &
alert=$!
wait_for '^alert\[[0-9]+\] I start ' "$ALOG" 3000 || fail "phase 2: alert did not start"
DLBY_LAT_DIR="$LAT" "$B/fuse-hub" 2>"$LOG2" &
hub=$!
wait_for '^fuse-hub\[[0-9]+\] I start sock=.* fuse=tracks hold_ms=500$' "$LOG2" 3000 ||
	fail "phase 2: hub did not start in tracks mode"
FAKE_PASS_S=8 "$B/tools/fake-sensors" radar 30 2>"$T/p_radar.log" &
radar=$!
FAKE_PASS_S=8 FAKE_PASS_LEFT_OFFSET_S=0 "$B/tools/fake-sensors" radar_left 30 2>"$T/p_left.log" &
left=$!
FAKE_PASS_S=8 "$B/tools/fake-sensors" sonar 30 2>"$T/p_sonar.log" &
sonar=$!

# First right pass: IN_ZONE at ~5.1 s, CLEAR at ~6.6 s; the second starts at 8 s.
# The left pass (radar only) ends in CLEAR at ~7 s.
wait_for '^fuse-hub\[[0-9]+\] I state side=right from=IN_ZONE to=' "$LOG2" 9000 ||
	fail "phase 2: right side never left IN_ZONE"
wait_for '^fuse-hub\[[0-9]+\] I state side=left from=APPROACHING to=CLEAR$' "$LOG2" 2000 ||
	fail "phase 2: left side never cleared"
wait_for '^fuse-hub\[[0-9]+\] I state side=right from=CLEAR to=APPROACHING$' "$LOG2" 4000 ||
	fail "phase 2: no second right approach"
# Sonar lost in the middle of an approach: the radar alone warns, degraded.
kill -KILL "$sonar" || fail "phase 2: sonar sender already gone"
wait "$sonar" 2>/dev/null
sonar=""
wait_for '^fuse-hub\[[0-9]+\] I state side=right from=APPROACHING to=DEG_APPROACHING$' "$LOG2" 2000 ||
	fail "phase 2: sonar loss did not give DEG_APPROACHING"
wait_for '^alert\[[0-9]+\] I pattern side=right state=DEG_APPROACHING ' "$ALOG" 2000 ||
	fail "phase 2: alert did not show DEG_APPROACHING"

kill -TERM "$hub" || fail "phase 2: hub already gone"
wait "$hub"
rc=$?
hub=""
[ "$rc" -eq 0 ] || fail "phase 2: hub exited $rc"
wait_for '^alert\[[0-9]+\] W hub_lost ' "$ALOG" 2000 || fail "phase 2: alert never noticed the hub gone"
kill -TERM "$radar" "$left" 2>/dev/null
wait "$radar" "$left" 2>/dev/null
radar=""
left=""
kill -TERM "$alert" || fail "phase 2: alert already gone"
wait "$alert"
rc=$?
alert=""
[ "$rc" -eq 0 ] || fail "phase 2: alert exited $rc"

bad=$(grep -v -E '^fuse-hub\[[0-9]+\] [IWE] [a-z_]+( [a-z][a-z0-9_]*=[^ ]*)*$' "$LOG2")
[ -z "$bad" ] || fail "phase 2: malformed log line: $bad"
right=" $(states right "$LOG2")"
left_s=" $(states left "$LOG2")"
echo "$right" | grep -q -E ' APPROACHING IN_ZONE (APPROACHING )?CLEAR APPROACHING DEG_APPROACHING ' ||
	fail "phase 2: right states:$right"
echo "$left_s" | grep -q -E ' APPROACHING CLEAR ' || fail "phase 2: left states:$left_s"
grep -q -E '^fuse-hub\[[0-9]+\] W degraded lost=sonar reason=eof$' "$LOG2" ||
	fail "phase 2: no degraded lost=sonar"

apat=" $(sed -n 's/^alert\[[0-9]*\] I pattern side=right state=\([A-Z_]*\) .*/\1/p' "$ALOG" | tr '\n' ' ')"
echo "$apat" | grep -q -E ' APPROACHING IN_ZONE .*DEG_APPROACHING ' ||
	fail "phase 2: alert right patterns:$apat"
grep -q -E '^alert\[[0-9]+\] I pattern side=left state=APPROACHING ' "$ALOG" ||
	fail "phase 2: alert never showed the left approach"
[ "$(grep -c -E '^alert\[[0-9]+\] I hub_up$' "$ALOG")" -eq 1 ] || fail "phase 2: hub_up not once"
[ "$(grep -c -E '^alert\[[0-9]+\] W hub_lost ' "$ALOG")" -eq 1 ] ||
	fail "phase 2: hub_lost while the hub was running"
ms=$(field silent_ms "$(grep -E '^alert\[[0-9]+\] W hub_lost ' "$ALOG")")
[ "$ms" -ge 1000 ] && [ "$ms" -le 1500 ] || fail "phase 2: hub_lost after $ms ms"
c=$(grep -E '^fuse-hub\[[0-9]+\] I counts .* final=1$' "$LOG2")
[ "$(field fuse "$c")" = tracks ] && [ "$(field right_in_zone "$c")" -ge 1 ] &&
	[ "$(field left_approach "$c")" -ge 1 ] && [ "$(field alerts "$c")" -gt 0 ] &&
	[ "$(field alert_dropped "$c")" = 0 ] || fail "phase 2: hub counts: $c"
# Latency: capture -> hub for every sensor, origin -> outputs at alert (sampled on
# each rise in urgency: right APPROACHING and IN_ZONE, left APPROACHING, at least).
for s in cap_radar cap_sonar cap_radar_left; do
	latl=$(grep -E "^fuse-hub\[[0-9]+\] I latency name=$s .* final=1$" "$LOG2")
	[ -n "$latl" ] && [ "$(field n "$latl")" -gt 0 ] && [ "$(field neg "$latl")" = 0 ] ||
		fail "phase 2: hub latency $s: $latl"
	ls "$LAT"/fuse-hub-$s-*.hist >/dev/null 2>&1 || fail "phase 2: no fuse-hub $s histogram"
done
latl=$(grep -E '^alert\[[0-9]+\] I latency name=origin_gpio .* final=1$' "$ALOG")
[ -n "$latl" ] && [ "$(field n "$latl")" -ge 2 ] && [ "$(field neg "$latl")" = 0 ] ||
	fail "phase 2: alert latency: $latl"
for h in origin_gpio hub_gpio; do
	ls "$LAT"/alert-$h-*.hist >/dev/null 2>&1 || fail "phase 2: no alert $h histogram"
done
alat=$(echo "$latl" | sed 's/^.* I latency name=origin_gpio //; s/ final=1$//')
cat "$LOG2" "$ALOG" | grep -q -E 'AddressSanitizer|LeakSanitizer|runtime error' &&
	fail "phase 2: sanitizer report"

# ---- phase 3: presence mode starts; bad fusion settings exit 78.
DLBY_FUSE_RADAR=presence "$B/fuse-hub" 2>"$T/hub3.log" &
hub=$!
wait_for '^fuse-hub\[[0-9]+\] I start sock=.* fuse=presence hold_ms=500$' "$T/hub3.log" 3000 ||
	fail "phase 3: no presence-mode start line"
kill -TERM "$hub" || fail "phase 3: hub already gone"
wait "$hub"
rc=$?
hub=""
[ "$rc" -eq 0 ] || fail "phase 3: hub exited $rc"
for kv in DLBY_FUSE_RADAR=bogus DLBY_FUSE_HOLD_MS=abc DLBY_FUSE_HOLD_MS=10001; do
	env "$kv" "$B/fuse-hub" 2>"$T/bad.log"
	[ $? -eq 78 ] || fail "phase 3: fuse-hub $kv: want 78"
	grep -q 'bad_config DLBY_FUSE_RADAR=' "$T/bad.log" || fail "phase 3: $kv: no bad_config line"
done

echo "hub_e2e: ok (radar=$n radar_left=$l sonar=$m right=[$(echo $right)] left=[$(echo $left_s)] hub_lost_ms=$ms alert_origin_gpio=[$alat])"
