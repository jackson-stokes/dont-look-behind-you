#!/bin/sh
# End-to-end for mechanism C (DESIGN.md §2 C): the scheduling policy dlby-super
# gives itself and every child, on first start and on restart. Usage: sched_e2e.sh <builddir>
#
# The real store, fuse-hub and alert (no GPIO) and fake-sensors as the three cap-*.
#   With CAP_SYS_NICE (super logs no sched_failed): the kernel reports FIFO 90 for
#   super, 80 alert, 70 fuse-hub, 60 cap-*, and OTHER with nice 5 for store, in
#   /proc/<pid>/stat and in the report_child lines.
#   Without it (plain Docker): prints "SKIP fifo checks", then still checks that
#   every real-time child logged sched_failed and ran anyway, and that nothing
#   inherited a real-time policy (all OTHER, store at nice 5).
#   Either way: fuse-hub and cap-sonar killed and restarted get their policy set
#   again (child_start sched=..., and FIFO again or sched_failed again).
# Then DLBY_SCHED=other: no sched_failed, everything OTHER. DLBY_SCHED=bogus: exit 78.
set -u
B=$(cd "$1" && pwd) || exit 1
T=$(mktemp -d /tmp/dlby-sched-e2e-XXXXXX) || exit 1
export DLBY_RUN_DIR="$T/run" DLBY_LOG_DIR="$T/log" DLBY_BIN_DIR="$T/bin"
export DLBY_ALERT_CHIP=none DLBY_REPORT_S=1
LOG="$T/super.log"
LOG2="$T/super2.log"
KIDS="store fuse-hub alert cap-radar cap-radar-left cap-sonar"
RT_KIDS="fuse-hub alert cap-radar cap-radar-left cap-sonar"
super=""

all_pids() {
	cat "$LOG" "$LOG2" 2>/dev/null |
		sed -n 's/^dlby-super\[[0-9]*\] I child_start name=[^ ]* pid=\([0-9]*\).*/\1/p'
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
	echo "sched_e2e: FAIL: $*"
	for f in "$LOG" "$LOG2"; do
		[ -f "$f" ] && { echo "---- $f"; cat "$f"; }
	done
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

pid_of() {
	sed -n "s/^dlby-super\[[0-9]*\] I child_start name=$1 pid=\([0-9]*\).*/\1/p" "$2" | tail -n 1
}

# "<policy> <rt_priority> <nice>" from /proc/<pid>/stat (fields 41, 40, 19;
# after the ") " the first field is 3, so field N is $(N-2)).
kstat() {
	sed 's/^.*) //' "/proc/$1/stat" 2>/dev/null | awk '{ print $39, $38, $17 }'
}

# want <pid> <policy> <rtprio> [nice] <what>
want() {
	got=$(kstat "$1")
	exp="$2 $3"
	[ -n "${5:-}" ] && exp="$exp $4"
	[ -n "${5:-}" ] || got=$(echo "$got" | cut -d' ' -f1-2)
	[ "$got" = "$exp" ] || fail "${5:-$4}: kernel has '$got', want '$exp'"
}

wait_up() {
	for n in $KIDS; do
		wait_count "^dlby-super\[[0-9]+\] I first_heartbeat name=$n " "$1" 1 3000 ||
			fail "no first_heartbeat for $n"
	done
}

mkdir -p "$T/bin" || fail "mkdir"
for n in store fuse-hub alert; do
	ln -s "$B/$n" "$T/bin/$n" || fail "symlink $n"
done
for n in cap-radar cap-radar-left cap-sonar; do
	ln -s "$B/tools/fake-sensors" "$T/bin/$n" || fail "symlink $n"
done

# ---- fifo (the default)
"$B/dlby-super" 2>"$LOG" &
super=$!
wait_up "$LOG"
grep -q -E '^dlby-super\[[0-9]+\] I start .* sched=fifo rt_runtime_us=[-0-9]+ ' "$LOG" ||
	fail "start line lacks sched=fifo rt_runtime_us="
for n in $KIDS; do
	case $n in
	store) s=other ;;
	fuse-hub) s=fifo:70 ;;
	alert) s=fifo:80 ;;
	*) s=fifo:60 ;;
	esac
	grep -q -E "^dlby-super\[[0-9]+\] I child_start name=$n pid=[0-9]+ sched=$s$" "$LOG" ||
		fail "child_start for $n lacks sched=$s"
done

if grep -q -E '^dlby-super\[[0-9]+\] W sched_failed name=dlby-super ' "$LOG"; then
	priv=0
	echo "sched_e2e: SKIP fifo checks (no CAP_SYS_NICE)"
	for n in $RT_KIDS; do
		grep -q -E "^dlby-super\[[0-9]+\] W sched_failed name=$n policy=fifo " "$LOG" ||
			fail "$n: FIFO refused but no sched_failed line"
		want "$(pid_of "$n" "$LOG")" 0 0 "$n (nothing inherited)"
	done
	want "$super" 0 0 "dlby-super"
else
	priv=1
	grep -q -E ' W sched_failed ' "$LOG" && fail "sched_failed with CAP_SYS_NICE"
	want "$super" 1 90 "dlby-super"
	want "$(pid_of fuse-hub "$LOG")" 1 70 "fuse-hub"
	want "$(pid_of alert "$LOG")" 1 80 "alert"
	for n in cap-radar cap-radar-left cap-sonar; do
		want "$(pid_of "$n" "$LOG")" 1 60 "$n"
	done
fi
want "$(pid_of store "$LOG")" 0 0 5 "store"

# The kernel's view in the soak report.
wait_count '^dlby-super\[[0-9]+\] I report_child n=2 name=cap-sonar ' "$LOG" 1 4000 ||
	fail "no report 2"
for n in dlby-super $KIDS; do
	l=$(grep -E "^dlby-super\[[0-9]+\] I report_child n=2 name=$n " "$LOG")
	p=$(field policy "$l")
	r=$(field rtprio "$l")
	if [ "$priv" = 1 ]; then
		case $n in
		store) e="other 0" ;;
		dlby-super) e="fifo 90" ;;
		fuse-hub) e="fifo 70" ;;
		alert) e="fifo 80" ;;
		*) e="fifo 60" ;;
		esac
	else
		e="other 0"
	fi
	[ "$p $r" = "$e" ] || fail "report_child for $n: policy=$p rtprio=$r, want $e"
done

# Restarts get their policy set again, never carried over.
for n in fuse-hub cap-sonar; do
	old=$(pid_of "$n" "$LOG")
	kill -KILL "$old" || fail "kill $n"
	wait_count "^dlby-super\[[0-9]+\] I first_heartbeat name=$n " "$LOG" 2 3000 ||
		fail "$n not restarted"
	new=$(pid_of "$n" "$LOG")
	[ "$new" != "$old" ] || fail "$n: no new pid"
	[ "$n" = fuse-hub ] && s=fifo:70 prio=70 || s=fifo:60 prio=60
	grep -q -E "^dlby-super\[[0-9]+\] I child_start name=$n pid=$new sched=$s$" "$LOG" ||
		fail "restarted $n: child_start lacks sched=$s"
	if [ "$priv" = 1 ]; then
		want "$new" 1 "$prio" "restarted $n"
	else
		[ "$(count "^dlby-super\[[0-9]+\] W sched_failed name=$n " "$LOG")" -ge 2 ] ||
			fail "restarted $n: policy not re-applied (one sched_failed only)"
		want "$new" 0 0 "restarted $n"
	fi
done
if [ "$priv" = 1 ]; then
	last=$(sed -n 's/^dlby-super\[[0-9]*\] I report n=\([0-9]*\) .*/\1/p' "$LOG" | tail -n 1)
	next=$((last + 1))
	wait_count "^dlby-super\[[0-9]+\] I report_child n=$next name=cap-sonar " "$LOG" 1 3000 ||
		fail "no report after the restarts"
	l=$(grep -E "^dlby-super\[[0-9]+\] I report_child n=$next name=fuse-hub " "$LOG")
	[ "$(field pid "$l") $(field policy "$l") $(field rtprio "$l")" = \
	  "$(pid_of fuse-hub "$LOG") fifo 70" ] || fail "report after restart: $l"
fi

kill -TERM "$super" || fail "super already gone"
wait "$super"
rc=$?
super=""
[ "$rc" -eq 0 ] || fail "dlby-super exited $rc"

# ---- DLBY_SCHED=other: the experiment's baseline, everything SCHED_OTHER.
DLBY_SCHED=other "$B/dlby-super" 2>"$LOG2" &
super=$!
wait_up "$LOG2"
grep -q -E '^dlby-super\[[0-9]+\] I start .* sched=other ' "$LOG2" || fail "other: start line"
grep -q -E ' W sched_failed ' "$LOG2" && fail "other: sched_failed logged"
want "$super" 0 0 "other: dlby-super"
for n in $RT_KIDS; do
	grep -q -E "^dlby-super\[[0-9]+\] I child_start name=$n pid=[0-9]+ sched=other$" "$LOG2" ||
		fail "other: child_start for $n lacks sched=other"
	want "$(pid_of "$n" "$LOG2")" 0 0 "other: $n"
done
want "$(pid_of store "$LOG2")" 0 0 5 "other: store"
kill -TERM "$super" || fail "other: super already gone"
wait "$super"
rc=$?
super=""
[ "$rc" -eq 0 ] || fail "other: dlby-super exited $rc"

DLBY_SCHED=bogus "$B/dlby-super" 2>"$T/bad.log"
[ $? -eq 78 ] || fail "DLBY_SCHED=bogus: want 78"
grep -q 'bad_config DLBY_SCHED=bogus' "$T/bad.log" || fail "no bad_config line"

cat "$LOG" "$LOG2" | grep -q -E 'AddressSanitizer|LeakSanitizer|runtime error' && fail "sanitizer report"

[ "$priv" = 1 ] && mode="fifo checked" || mode="fifo skipped"
echo "sched_e2e: ok ($mode; restarts re-applied; other mode all SCHED_OTHER)"
