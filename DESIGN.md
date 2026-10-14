# Design document (M2) -- Don't Look Behind You (Jackson Stokes, Gage Boullosa)

Hardware and pin map: [docs/HARDWARE.md](docs/HARDWARE.md). Problem statement: [PROBLEM.md](PROBLEM.md).

## 1. Architecture

![Architecture: processes, kernel components, hardware, and the rate on every link](docs/architecture.svg)

Seven processes on the Pi 5, all started and watched by `dlby-super`. Sensor data goes
straight from the capture processes to `fuse-hub`, which decides the alert state and
forwards a copy of everything to `store`. Unlike a sampler → storage → analysis pipeline,
storage is deliberately off the alert path, so a slow SD write can never delay an alert.

### Processes
| Process | Owner | Role | Scheduling |
|---|---|---|---|
| `dlby-super` | shared | fork/exec every child, `waitpid`, restart with backoff, kill a child that misses heartbeats for 5 s | SCHED_FIFO 90, asleep almost always |
| `cap-radar` | Gage | read `/dev/ttyAMA2`, frame-sync, parse targets, timestamp each frame | SCHED_FIFO 60 |
| `cap-sonar` | Gage | trigger GPIO23 on an absolute 60 ms deadline; read kernel-timestamped echo edges on GPIO24 from the gpio character device | SCHED_FIFO 60 |
| `fuse-hub` | Jackson | CLEAR → APPROACHING → IN_ZONE → CLEAR state machine; ignores targets that aren't approaching | SCHED_FIFO 70 |
| `alert` | shared | drive the LED (GPIO17) and buzzer (GPIO22); show a fault pattern if `fuse-hub` goes silent | SCHED_FIFO 80 |
| `store` | Jackson | append-only log of raw samples and state changes that survives a power cut | SCHED_OTHER, nice +5 |
| `status-lcd` | shared | 1602 LCD over I2C1 for bench and demo only. Separate from `alert` so a slow I2C write can't delay the alert | SCHED_OTHER |

Kernel components used: the RP1 UART and tty layer (radar), the gpio character device
with edge events stamped on `CLOCK_MONOTONIC` (ultrasonic, LED, buzzer), `i2c-dev` on
I2C1 (LCD), and ext4 on the SD card (`store`). The product makes no network calls and has
no LAN interface in this milestone.

### IPC
Every process-to-process link is a Unix domain socket of type `SOCK_SEQPACKET`, created
by `dlby-super` as a socketpair with `SOCK_CLOEXEC` and handed to the child at exec. We
chose it because it keeps message boundaries, a dead peer shows up immediately as
EOF/`EPOLLHUP`, and fds can't leak across restarts. Our rates are a few hundred bytes per
second, so shared memory's speed isn't needed.

### Links and rates
| Link | Transport | Payload | Rate | Bandwidth |
|---|---|---|---|---|
| radar → `cap-radar` | UART 115200 8N1 | frame = 10 B + payload | ~10 Hz | ≤1.3 KB/s (link capacity 11.5 KB/s) |
| `cap-sonar` → HC-SR04 | GPIO23 out | 10 µs trigger pulse | 16 Hz | — |
| HC-SR04 → `cap-sonar` | GPIO24 edge events | 2 edges per ping | 32 events/s | — |
| `cap-radar` → `fuse-hub` | UDS | `radar_msg` ~64 B (timestamp, seq, target count, targets) | 10 Hz | ~640 B/s |
| `cap-sonar` → `fuse-hub` | UDS | `sonar_msg` 24 B (timestamp, seq, cm or timeout) | 16 Hz | ~400 B/s |
| `fuse-hub` → `alert` | UDS | `alert_msg` 16 B (state, timestamp) | on change + 2 Hz keepalive | <50 B/s |
| `fuse-hub` → `store` | UDS | raw messages forwarded, plus state changes | ~26 Hz | ~1.1 KB/s (~4 MB/h, ~190 MB per 48 h soak) |
| `fuse-hub` → `status-lcd` | UDS | status snapshot ~64 B | 2 Hz | ~130 B/s |
| each child → `dlby-super` | UDS, one per child | heartbeat 8 B | 1 Hz | 8 B/s each |
| `status-lcd` → LCD | I2C1 at 100 kHz | 2×16 characters | 2 Hz | — |

Two numbers to confirm on hardware:
- **Ultrasonic rate.** 16 Hz, not the "~20 Hz" in HARDWARE.md. The HC-SR04 needs at least
  60 ms between pings so it doesn't hear the previous ping's echo, and the bench sketch
  already uses 60 ms.
- **Radar payload size** is an estimate until the LD2451 frame format is checked against
  the datasheet and real captures.

## 2. Mechanism mapping

### C: SCHED_FIFO on the alert chain (`cap-radar`, `cap-sonar`, `fuse-hub`, `alert`)
**Not the justification: range accuracy.** The kernel timestamps echo edges in the GPIO
interrupt path, so the measured range, (t_fall − t_rise) × 343 m/s ÷ 2, doesn't depend on
when `cap-sonar` gets to run. The trigger time doesn't matter either, because the echo's
rising edge is the reference. Scheduling could only cost range data if the kernel's
16-event edge buffer overflowed, which takes more than about 250 ms of starvation at
32 events/s.

**The justification:**
1. **Alert latency under load.** A sensor reading reaches the buzzer through three
   wakeups: capture, `fuse-hub`, `alert`. Under the default scheduler, with `store`
   flushing to the SD card and other load on the CPU, each wakeup can slip by milliseconds
   to tens of milliseconds. At a 30 km/h closing speed (8.3 m/s), 50 ms is about 0.4 m of
   cyclist travel inside a 4 m zone. The whole chain runs SCHED_FIFO, with priority rising
   downstream (capture 60 < `fuse-hub` 70 < `alert` 80) so each stage runs as soon as its
   input arrives.
2. **Regular trigger period.** A late wakeup stretches the gap between pings, which adds
   directly to the worst-case detection delay. Periodic wakeups use absolute deadlines
   (`clock_nanosleep` with `TIMER_ABSTIME`) so lateness doesn't accumulate.
3. **Radar timestamps.** UART bytes are buffered by the kernel and timestamped in user
   space when read, so radar timestamps do shift with scheduling delay. The fusion check
   that the ultrasonic hit matches the radar's last track depends on them.

**Keeping C safe:** every real-time process calls `mlockall`. The kernel's real-time
throttle (`sched_rt_runtime_us`, 950 ms per second) stays at its default, so a real-time
child stuck in a loop can't starve the system. `dlby-super` has the highest priority and
almost always sleeps, so it can still detect and kill a stuck child.

### D: crash-consistent storage
TODO

### E: multi-process architecture with a supervisor
TODO

## 3. Failure modes
TODO

## 4. Storage and data
TODO

## 5. Constraints and substitutions
TODO. Known so far: stock Pi OS is not a PREEMPT_RT kernel, so SCHED_FIFO bounds
scheduling delay but not delay from non-preemptible kernel sections.

## 6. Evaluation plan

### C: does SCHED_FIFO bound alert latency?
Run the same workload with the alert chain under SCHED_OTHER and under SCHED_FIFO, at
three loads: idle; `stress-ng --cpu 4`; and `stress-ng --io 2 --vm 1` with `store`
writing. Each run collects at least 10,000 events.

| # | Measures | Method | Target (SCHED_FIFO) |
|---|---|---|---|
| C1 | Added latency from sensor event to alert GPIO edge | Each message carries its origin timestamp (kernel echo edge, or radar read). The alert GPIO is wired back to a spare input GPIO, so the kernel timestamps its edge on the same clock. A software-only variant replays recorded input and has `alert` log `CLOCK_MONOTONIC` after the write | p99 ≤ 2 ms, max ≤ 10 ms under load; SCHED_OTHER reported alongside |
| C2 | Jitter in the trigger period | Kernel timestamps of the trigger edge via the same loopback, or of echo rising edges | standard deviation ≤ 100 µs, longest gap ≤ 61 ms |
| C3 | Whether scheduling changes range readings (expected: no) | Static target at a taped distance, SCHED_OTHER vs SCHED_FIFO, under load | mean and standard deviation unchanged |

The sensor periods (60 ms ultrasonic, 100 ms radar) dominate end-to-end detection time and
are reported separately. C controls only the part that scheduling adds.

## 7. Ownership map
TODO

## 8. AI-use plan
TODO
