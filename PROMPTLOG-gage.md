# PROMPTLOG: Gage Boullosa

Annotated episodes from my Claude Code sessions (CS370 term project §10, §12).
Each one names what I asked, what the agent did, what I or the evidence pushed
back on, and what changed. Transcripts: `~/.claude/projects/*dont-look-behind-you*/`,
copied out at each milestone.

> DRAFT. The facts and commit references below are filled in from the session.
> Every **My take** line is mine to write; it is what gets defended on demo day.

| # | Episode | Required type |
|---|---|---|
| 1 | From a bike to a car | revised plan |
| 2 | Two radars vs. two cooperating sensors | revised plan |
| 3 | Garbled serial monitor | tool-output debugging loop |
| 4 | "RADAR no data" that wasn't true | tool-output debugging loop with real hardware evidence; rejected diff |
| 5 | The radar is slower than the hub expected | evidence across the ownership boundary |
| 6 | fuse-hub ignores sonar flags | review of my partner's work |
| 7 | Restarts that seemed to be missing | tool-output debugging loop with real hardware evidence |
| 8 | config.txt sections on the Pi | tool-output debugging loop |

---

## 1. From a bike to a car (revised plan)
**Asked:** plan the hardware and inventory for a bike handlebar device that warns of
cars approaching from behind.
**Agent:** produced a plan; I rejected it twice ("don't write any code yet at all",
"we are still in planning stages"), then asked about running on an ESP32 with an
18650 battery shield for size.
**Pushback:** the agent pointed out that the handout requires a Raspberry Pi as the
product board, so an ESP32 could only be a bench tool. Size and power on a bike
made a Pi impractical.
**Changed:** the product moved to a car (Pi 5 in the cabin, sensor pod on the right
rear quarter) and the problem became the right-hook. The ESP32 stayed for bench tests.
Evidence: docs/HARDWARE.md (175c2b6), PROBLEM.md.
**My take:**

## 2. Two radars vs. two cooperating sensors (revised plan)
**Asked:** sensors for the car version.
**Agent:** warned that two independent radars would not meet §3.1, which requires
the sensors to cooperate, not merely coexist.
**Changed:** an LD2451 radar (rearward, long range) fused with an HC-SR04
(sideways, blind-spot zone), handing off CLEAR -> APPROACHING -> IN_ZONE.
Later I asked for one pod per side; the agent kept each pod as radar + ultrasonic
so cooperation holds per side, and flagged it as a design change to agree with Jackson.
**My take:**

## 3. Garbled serial monitor (tool-output debugging loop)
**Saw:** unreadable characters in the Arduino IDE serial monitor on Windows.
**Agent:** matched it to a baud mismatch; the sketch logs at 115200.
**Changed:** set the monitor to 115200; output readable on the next capture.
**My take:**

## 4. "RADAR no data" that wasn't true (hardware evidence; rejected diff)
**Evidence:** bench capture (docs/evidence/sensor-check.txt): `RADAR no data`
warnings appeared right after valid frames, and the target LED (D26) never lit.
**Agent:** traced it to `loop()` taking `millis()` before `radar_poll()`; frames
stamped later make `now - last_*` underflow to a huge number. Earlier it had also
found D26 reading the frame tail on empty frames (fixed in e450720).
**Rejected:** the agent proposed moving `now = millis()` after `radar_poll()`. I
rejected that plan and asked for a full project review first; the fix is still
deferred. The note in docs/evidence/pi-sensor-smoke.txt records that those lines
are a sketch artifact, not a radar fault.
**My take:**

## 5. The radar is slower than the hub expected (evidence across the boundary)
**Evidence:** empty LD2451 frames arrive every ~1.39 s with no target (bench), and
every ~1.44 s on the Pi (docs/evidence/m3-capture-pi.txt).
**Problem:** fuse-hub's radar loss timeout was 1 s, so a healthy radar with nothing
in view would have been declared lost.
**Changed:** Jackson raised it to 3 s from the bench evidence (e425b77).
**My take:**

## 6. fuse-hub ignores sonar flags (review of my partner's work)
**Questioned:** while planning cap-sonar, the agent read fuse-hub.c and found the
hub counts any SONAR message as "alive" and never reads `flags`. A dead sensor
reporting `SONAR_NO_ECHO` every ping would never be declared lost.
**Decided:** cap-sonar sends nothing when there is no echo edge or the line is stuck
high, so the hub's 8-ping staleness detects it. An echo wider than 25 ms is still
sent with `SONAR_NO_ECHO` (alive, nothing in range). Alternative for Jackson: have
the hub read the flags.
**Answer from Jackson:** (pending; PR review)
**Changed:** cap-sonar (0590067); verified on the Pi: open air gave `far=11`,
`missed=0`, no false `sonar_lost`.
**My take:**

## 7. Restarts that seemed to be missing (hardware evidence)
**Evidence:** first demo dry run: `child_restart name=fuse-hub in_ms=100` with no
`child_start` after it, then `stop signal=TERM`; only ~8 s of records.
**Agent:** did not call it a supervisor bug. Hypothesis: the SEGV, KILL and TERM
were pasted together and landed within milliseconds, so super stopped before the
100 ms restart fired. Asked for a rerun with the kills spaced out and the full log.
**Result:** rerun showed a new fuse-hub pid (2832), cap-sonar restarted
(`child_start`, `first_heartbeat`, `sonar_up`, `healthy`), store exited last,
`bad=0 contiguous=1`. Not a bug.
**My take:**

## 8. config.txt sections on the Pi (tool-output debugging loop)
**Evidence:** `grep` of /boot/firmware/config.txt showed bracketed sections and
existing overlays.
**Agent:** an appended line lands in whatever section is last and may not apply to
a Pi 5, so it added `[all]` before `dtoverlay=uart2-pi5`.
**Result:** `pinctrl get 4-5` showed `TXD2`/`RXD2`; `/dev/ttyAMA2` present
(docs/evidence/pi-sensor-smoke.txt).
**My take:**
