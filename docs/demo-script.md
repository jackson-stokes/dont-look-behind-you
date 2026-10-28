# M3 demo video: script and walkthrough (Gage)

About 4 minutes on screen. Two Mac Terminal windows side by side, both SSH'd into the Pi:
**T1** (left) runs the supervisor and is never typed into after Step 2; **T2** (right) is
where every command goes.

Type or paste **one command at a time**. Copy only the grey command boxes, never the
"Wait for" or "Say" text. **Never press Ctrl-C in T1.**

---

## Part A: off camera (about 5 min)

**A1. Hardware**
- Pi powered, both sensors wired. Press the HC-SR04 jumpers and the divider leads firmly into
  the breadboard.
- Ultrasonic pointed at a wall or box 0.5–1 m away.
- Both radars wired: **right** radar TX on **pin 29**, **left** radar TX on **pin 33**. Route
  the pin 29 wire so you can pull it **without reaching across the sonar wiring**. Pull it by
  its plastic connector.
- `ls -l ~/dlby-demo/bin` shows a `cap-radar-left` link next to `cap-radar` (same target).

**A2. T1: build and test, before any exports**
```sh
cd ~/dlby-src && git checkout gagebranch && git pull && make && make test 2>&1 | tail -3
```
Wait for: `all tests passed (build)`

**A3. T1: sonar sanity check**
```sh
gpiomon -c gpiochip0 -e both -n 2 24 &
sleep 0.5; gpioset -c gpiochip0 -t 20us,0 23=1; wait
```
Wait for: a `rising` line and a `falling` line. If nothing prints, press Ctrl-C and reseat
the sonar wiring (power off first).

**A4. Both T1 and T2: demo settings**
```sh
cd ~/dlby-src
export DLBY_RUN_DIR=$HOME/dlby-demo/run DLBY_LOG_DIR=$HOME/dlby-demo/log DLBY_BIN_DIR=$HOME/dlby-demo/bin
export DLBY_CHILDREN=store,fuse-hub,cap-radar,cap-radar-left,cap-sonar
```

**A5. T2: nothing left over**
```sh
pgrep -a 'dlby-super|store|fuse-hub|cap-'
```
Wait for: no output. If something is listed, run `pkill -x dlby-super; sleep 2` and check again.

**A6. T1: empty log, clean screens**
```sh
rm -rf ~/dlby-demo/log ~/dlby-demo/run ~/dlby-demo/super.log; clear
```
Then run `clear` in T2. Press ⌘+ in both windows until the text is easy to read.

---

## Part B: recording

Press **⌘⇧5** → **Record Selected Portion** → box both windows → **Options** → your mic →
**Record**.

### Scene 1: intro (T2, ~20 s)
> **Say:** "This is Don't Look Behind You, by Gage Boullosa and Jackson Stokes, for CS370. It's
> a car-mounted blind-spot warning: an LD2451 radar looking back along each side of the car,
> and an HC-SR04 ultrasonic looking sideways on the right. This is milestone 3: both sensors through the real
> pipeline into real storage, with our supervisor mechanism live."

```sh
git log -1 --oneline; ls -l ~/dlby-demo/bin
```
> **Say:** "These links are what the supervisor runs. cap-radar reads a radar over UART, and
> runs twice, once per side; cap-sonar drives the ultrasonic over GPIO. These are the real capture processes, not
> simulated sensors."

### Scene 2: start the system (T1, ~25 s)
```sh
build/dlby-super 2>&1 | tee ~/dlby-demo/super.log
```
Wait for: five `child_start` lines, five `first_heartbeat` lines, `sensor_up` for `radar`,
`radar_left` and `sonar`, then `fuse-hub … I healthy`.
> **Say:** "The supervisor starts the store, the fusion hub, and one process per sensor (the
> right radar, the left radar and the ultrasonic) as separate processes. Each one heartbeats back over its own socket. The hub has seen both
> sensors and reports healthy."

### Scene 3: data reaching storage (T2, ~25 s)
```sh
build/tools/dlby-logcat ~/dlby-demo/log
```
Count to five, then run it again:
```sh
build/tools/dlby-logcat ~/dlby-demo/log
```
Wait for: `radar=`, `radar_left=` and `sonar=` all higher the second time.
> **Say:** "Every sensor reading goes from the capture process to the hub to the store, which
> writes it to disk as a CRC-checked record. Sonar arrives about sixteen times a second. With
> no target in view, the radar sends a frame about every 1.4 seconds."

### Scene 4: crash the hub (T2, ~25 s)
```sh
kill -SEGV $(pgrep -x fuse-hub)
```
Wait for, in T1: `child_exit name=fuse-hub signal=SEGV` → `child_restart` → `child_start
name=fuse-hub` → both `sensor_up` → `healthy`.
> **Say:** "I just crashed the hub with a segfault. The supervisor caught it, restarted it a
> hundred milliseconds later, and both sensors reconnected on their own. Each sensor lost one
> message while the hub was down, and that's logged as a send_dropped."

### Scene 5: crash a sensor process (T2, ~25 s)
Make sure T1 shows `healthy` first.
```sh
kill -KILL $(pgrep -x cap-sonar)
```
Wait for, in T1: `degraded lost=sonar reason=eof` → `child_exit name=cap-sonar signal=KILL` →
`child_start name=cap-sonar` → `sonar_up` → `sensor_up name=sonar` → `healthy`.
> **Say:** "Now the sonar process is killed. The hub reports degraded immediately, so the
> driver is never told everything's fine when it isn't. The supervisor restarts the process
> and the system is healthy again."

### Scene 6: unplug the radar (hands, ~30 s)
Make sure T1 shows `healthy` first. No command: **pull the radar's TX wire from pin 29.**
> **Say:** "This is a real hardware fault. I've pulled the radar's data line."

Wait for, in T1 (3–5 s): `cap-radar … W radar_silent`, then
`fuse-hub … W degraded lost=radar reason=stale`.
> **Say:** "Within three seconds the hub notices the radar has gone silent and reports
> degraded. It keeps running on the ultrasonic alone."

**Plug the wire back into pin 29.** Wait for: `radar_back`, `sensor_up name=radar`, `healthy`.
> **Say:** "And when it's reconnected, the radar comes back and the system recovers."

### Scene 7: orderly shutdown (T2, ~20 s)
```sh
kill -TERM $(pgrep -x dlby-super)
```
Wait for, in T1: `stop signal=TERM` → exits with `code=0` → `store … I exit code=0` →
`child_exit name=store code=0` → `dlby-super … I exit code=0` → the prompt returns.
> **Say:** "An orderly shutdown. The supervisor stops the sensors and the hub first, and the
> store last, so everything the hub sent is saved."

### Scene 8: proof (T2, ~20 s)
```sh
build/tools/dlby-logcat ~/dlby-demo/log
```
Hold on the line for about 3 seconds. Point at `bad=0` and `contiguous=1`.
> **Say:** "Across a hub crash, a sensor crash and an unplugged radar: zero damaged records,
> and no gaps in the sequence numbers. Both real sensors through the real pipeline into real
> storage, with the supervisor mechanism live. That's milestone 3."

Click **stop** in the menu bar.

---

## Part C: after recording
1. Watch the video once, start to finish: is the text readable, and is the audio there?
2. Over about 100 MB? Open it in QuickTime → **File → Export As → 720p**.
3. T2: paste this output to Claude for the evidence file:
   ```sh
   cat ~/dlby-demo/super.log; build/tools/dlby-logcat ~/dlby-demo/log
   ```

## If something goes wrong
| What happened | What to do |
|---|---|
| Typed into T1 by mistake | Nothing ran. Carry on in T2. |
| Pressed Ctrl-C in T1 | Stop recording. T2: `pkill -x dlby-super`, then redo A5–A6 and start over. |
| No `healthy` after Scene 2 | Stop recording. Paste T1's output to Claude; don't change anything. |
| `sonar_lost` you didn't cause | A sonar wire got bumped. Stop recording, power off, reseat, redo A3. |
| Radar doesn't come back | Check the wire is on pin 29 and wait about 2 s for the next frame. |
| Anything else odd | Stop, and start again from A5. Every take starts from an empty log. |
