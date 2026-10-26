# M3 demo video: runbook

For Gage, recording alone on the Pi. It takes about 15 minutes, plus the recording itself.

The video has to show two things:
1. **Both sensors flowing through the real pipeline into the store**: cap-radar and cap-sonar,
   then `fuse-hub`, then `store`, with record counts rising on disk.
2. **Mechanism E**: `dlby-super` restarting a crashed process while the data keeps flowing.

Everything runs in the foreground in a terminal as your normal user, so there's no sudo and
no systemd. All state goes under `~/dlby-demo/`.

## 1. Checklist: what must be on camera
- [ ] `git log -1 --oneline` and `ls -l ~/dlby-demo/bin`, so the viewer knows which build and
      which sensors (real or simulated) they're watching
- [ ] `child_start` for `store`, `fuse-hub`, `cap-radar` and `cap-sonar`, then a
      `first_heartbeat` for each
- [ ] `fuse-hub[…] I sensor_up name=radar`, `… name=sonar`, then `I healthy`
- [ ] `dlby-logcat` with `radar=` and `sonar=` counts rising
- [ ] `kill -SEGV` on fuse-hub, then `E child_exit name=fuse-hub signal=SEGV`, then
      `I child_restart name=fuse-hub in_ms=100`, then a new `fuse-hub[…] I start`, then
      `sensor_up` again
- [ ] the counts still rising after the restart
- [ ] a clean stop: `child_exit name=store` last, then `dlby-super[…] I exit code=0`
- [ ] the final `dlby-logcat` showing `bad=0 … contiguous=1`, with radar > 0 and sonar > 0

## 2. One-time setup
```sh
git clone https://github.com/jackson-stokes/dont-look-behind-you ~/dlby-src
cd ~/dlby-src
git checkout main && git pull
```
Use `main` if it already has the supervisor; otherwise use the M3 branch, which has
everything. Either works for the demo.
```sh
[ -f src/super/dlby-super.c ] || git checkout jackson-m3-super
git log -1 --oneline
```
Build, and run the tests once on the Pi itself:
```sh
make && make test
```
The last line should be `all tests passed (build)`. It takes about 30 s, mostly the
end-to-end tests. Skip `make asan` here; it isn't needed for the demo.

## 3. Pick the sensors
Make the demo directory first:
```sh
mkdir -p ~/dlby-demo/bin
ln -sf ~/dlby-src/build/store    ~/dlby-demo/bin/store
ln -sf ~/dlby-src/build/fuse-hub ~/dlby-demo/bin/fuse-hub
```
`dlby-super` runs each child as `$DLBY_BIN_DIR/<name>`. The names must be exactly `cap-radar`
and `cap-sonar`.

### A. Your real capture processes (preferred)
Your `cap-radar` and `cap-sonar` work under the supervisor only if they:
- connect with `ipc_conn_init(&c, "fuse-hub")` and send `struct radar_msg` / `struct sonar_msg`
  from `src/common/msg.h`, `memset` to 0 first, with `ipc_send()`
- call `hb_init()` once at start, then `hb_tick(&hb, now)` and `hb_readable(&hb)` on every pass
  of the main loop (`src/common/hb.h`). Without heartbeats, super logs `heartbeat_lost` and
  kills them about 10 s after each start. `tools/fake-sensors.c` (demo mode) is a working
  example.
- exit 0 on SIGTERM, and also when `hb_tick`/`hb_readable` return -1 (super gone)
- have the hardware set up as in `docs/HARDWARE.md`: `dtoverlay=uart2-pi5` for the radar on
  GPIO 4/5, and the 1k/2k divider on ECHO (GPIO 24)

Then:
```sh
ln -sf /path/to/your/cap-radar ~/dlby-demo/bin/cap-radar
ln -sf /path/to/your/cap-sonar ~/dlby-demo/bin/cap-sonar
```

### B. Fallback: simulated sensors
If the capture processes aren't ready, use `fake-sensors`. It sends radar at 10 Hz and sonar
at 16 Hz through the same IPC, hub and store, and heartbeats like a real child:
```sh
ln -sf ~/dlby-src/build/tools/fake-sensors ~/dlby-demo/bin/cap-radar
ln -sf ~/dlby-src/build/tools/fake-sensors ~/dlby-demo/bin/cap-sonar
```
**If you use B, say so in the video**, for example: "the sensors here are simulated by
fake-sensors; everything downstream is the real pipeline". The `ls -l` shot shows the
symlinks too. Don't present simulated data as the real radar and ultrasonic.

## 4. Lay out the screen
Use two terminals side by side, either two SSH windows or tmux (`tmux`, then `Ctrl-b %`). In
both, run:
```sh
cd ~/dlby-src
export DLBY_RUN_DIR=$HOME/dlby-demo/run DLBY_LOG_DIR=$HOME/dlby-demo/log DLBY_BIN_DIR=$HOME/dlby-demo/bin
export DLBY_CHILDREN=store,fuse-hub,cap-radar,cap-sonar
```
`alert` and `status-lcd` aren't written yet, so `DLBY_CHILDREN` leaves them out. Otherwise
super would log `code=78` and `child_disabled` for each one.

To start from an empty log (recommended before each take):
```sh
rm -rf ~/dlby-demo/log ~/dlby-demo/run
```

## 5. Recording script
Start the screen recording now.

**Step 1: show what's running.** In terminal 2:
```sh
git log -1 --oneline
ls -l ~/dlby-demo/bin
```
Say which branch it is and whether the sensors are real (A) or simulated (B).

**Step 2: start the stack.** In terminal 1:
```sh
build/dlby-super 2>&1 | tee ~/dlby-demo/super.log
```
Within about a second you should see:
```
dlby-super[…] I start bin_dir=… children=store,fuse-hub,cap-radar,cap-sonar
dlby-super[…] I child_start name=store pid=…
dlby-super[…] I child_start name=fuse-hub pid=…
dlby-super[…] I child_start name=cap-radar pid=…
dlby-super[…] I child_start name=cap-sonar pid=…
dlby-super[…] I first_heartbeat name=… ms=…        (one per child)
store[…] I start dir=… segment=seg-000001.log next_seq=2
fuse-hub[…] I start sock=…
fuse-hub[…] I sensor_up name=radar
fuse-hub[…] I sensor_up name=sonar
fuse-hub[…] I healthy
```
The lines from different processes interleave, so the order varies. Two kinds of line near
the start are normal, because all four processes start at once:
- `cap-…[…] W send_dropped err=No such file or directory count=1`: a sensor tried to send
  before the hub's socket existed, and it retries by itself.
- `fuse-hub[…] I store_back dropped=N`: the hub forwarded a few records before store was
  listening, and it reports them as a GAP.

Say: "the supervisor starts every process in order; each one heartbeats back; the hub sees
both sensors".

**Step 3: show data reaching the store.** In terminal 2:
```sh
watch -n1 build/tools/dlby-logcat ~/dlby-demo/log
```
Point at `radar=` and `sonar=` going up every second. Each of those is a CRC-checked record
on disk.
- With real hardware and no target, the LD2451 sends about **one frame every 1.4 s**
  (`docs/evidence/sensor-check.txt`), so radar rises slowly. That's expected. Sonar rises by
  about 16 per second.
- Optional: put your hand in front of the ultrasonic. The count doesn't change, but the
  readings do.

Leave `watch` running.

**Step 4: crash the hub (mechanism E).** Open a third pane, or quit `watch` with Ctrl-C (only
`watch` is in that terminal, so that's safe), then:
```sh
kill -SEGV $(pgrep -x fuse-hub)
```
Terminal 1 should show:
```
dlby-super[…] E child_exit name=fuse-hub signal=SEGV
dlby-super[…] I child_restart name=fuse-hub in_ms=100
dlby-super[…] I child_start name=fuse-hub pid=…      (new pid)
fuse-hub[…] I start sock=…
dlby-super[…] I first_heartbeat name=fuse-hub ms=…
fuse-hub[…] I sensor_up name=radar
fuse-hub[…] I sensor_up name=sonar
fuse-hub[…] I healthy
```
A `cap-…[…] W send_dropped err=Broken pipe count=1` is expected: the sensors lost the messages
sent during the ~0.1 s the hub was down, then reconnected on their own.

Say: "the hub was killed with a segfault; the supervisor saw SIGCHLD, restarted it 100 ms later,
and both sensors reconnected". Then run `watch -n1 build/tools/dlby-logcat ~/dlby-demo/log`
again and show the counts still rising.

**Step 5 (optional): more of E.**
- `kill -KILL $(pgrep -x store)` restarts store. The next record in the log is a store GAP,
  and `dlby-logcat` still shows `contiguous=1`.
- `kill -KILL $(pgrep -x cap-sonar)` (a crash) gives
  `fuse-hub … W degraded lost=sonar reason=eof`, then `child_exit name=cap-sonar signal=KILL`,
  then a restart, then `sensor_up name=sonar`, then `healthy`. Use `-KILL`, not a plain `kill`.
  A plain `kill` sends SIGTERM, the sensor exits 0, and per DESIGN §2 E an exit 0 means
  "stopped on request", so super logs `child_stopped … restart=no` and leaves it stopped.

**Step 6: stop cleanly.** In terminal 2:
```sh
kill -TERM $(pgrep -x dlby-super)
```
**Don't press Ctrl-C in terminal 1.** It signals every process in that terminal at once, so the
children would exit on their own before super's ordered shutdown. Expect:
```
dlby-super[…] I stop signal=TERM
…child_exit lines for cap-radar, cap-sonar, fuse-hub…
store[…] I exit code=0 records=… syncs_timed=… syncs_state=…
dlby-super[…] I child_exit name=store code=0        (store last, so it saved everything)
dlby-super[…] I exit code=0
```

**Step 7: final proof.**
```sh
build/tools/dlby-logcat ~/dlby-demo/log
```
Hold the frame on a line like:
```
segments=1 records=… radar=… sonar=… alert=… gap_msg=… gap_dropped=… store_gap=… other=0 bad=0 first_seq=1 last_seq=… contiguous=1
```
`bad=0` means no damaged records were kept. `contiguous=1` means the sequence numbers have
no holes across restarts. Stop the recording.

## 6. After recording
Save the evidence and commit it on your branch:
```sh
mkdir -p docs/evidence
{ cat ~/dlby-demo/super.log; build/tools/dlby-logcat ~/dlby-demo/log; } > docs/evidence/m3-demo-pi.txt
git add docs/evidence/m3-demo-pi.txt
git commit -m "M3: Pi demo run evidence"
```
Note in the commit or PR whether it was real capture (A) or fake-sensors (B).

## 7. Troubleshooting
| Symptom | Meaning | What to do |
|---|---|---|
| `E exec_failed name=cap-… err=No such file or directory`, then `child_disabled … code=78` | the symlink in `~/dlby-demo/bin` is missing or broken | `ls -lL ~/dlby-demo/bin`, fix the link, restart super |
| `E heartbeat_lost name=cap-…` about 10 s after each start, then restarts | the capture process isn't calling `hb_init`/`hb_tick` | wire in `hb.h` (section 3A) |
| `fuse-hub … W degraded lost=radar reason=stale` over and over | radar frames more than 3 s apart | collect evidence before changing anything (CLAUDE.md): `dmesg \| tail -50`, `ls -l /dev/ttyAMA*`, and a bench capture as in `docs/evidence/sensor-check.txt` |
| radar never reaches `sensor_up` | no valid radar messages reach the hub | same evidence as above, plus `fuse-hub … W bad_msg` lines if there are any |
| `listen_failed` or `log_open_failed` | permissions or leftovers in `~/dlby-demo` | `rm -rf ~/dlby-demo/log ~/dlby-demo/run` and start again |
| `pgrep -x` returns more than one pid | a process left over from an earlier take | `pgrep -a fuse-hub`, kill the old one, retake |
| super is gone but children are still running | shouldn't happen: they exit within 1 s via PDEATHSIG | `pgrep -a 'store\|fuse-hub\|cap-'`, save the output as evidence and send it to Jackson |
