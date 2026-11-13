<img width="3024" height="4032" alt="IMG_1378" src="https://github.com/user-attachments/assets/25577a50-0d97-4ea7-9f39-721c9dab4f82" />

# Don't Look Behind You

A car-mounted blind-spot warning for cyclists. A Raspberry Pi 5 watches both
sides of the car with a 24 GHz radar looking back along each side, plus an
ultrasonic sensor that confirms a cyclist is right beside the car. It warns the
driver before they turn or change lanes across a cyclist they can't see, with
LEDs on the side mirrors like a factory blind-spot light.

CS370 term project, Colorado State University: Gage Boullosa and Jackson Stokes.

## Hardware
| Part | Interface | Pi 5 pins |
|---|---|---|
| HLK-LD2451 radar, right side | UART2 `/dev/ttyAMA2`, 115200 | TX→29 (GPIO5), RX→7 (GPIO4) |
| HLK-LD2451 radar, left side | UART4 `/dev/ttyAMA4`, 115200 | TX→33 (GPIO13), RX→32 (GPIO12) |
| HC-SR04 ultrasonic, beside the right radar | GPIO | TRIG→16 (GPIO23), ECHO→10k/20k divider→18 (GPIO24) |

`/boot/firmware/config.txt`, under `[all]`: `dtoverlay=uart2-pi5` and
`dtoverlay=uart4-pi5`. Full wiring: `docs/HARDWARE.md`.

## Architecture
Separate processes, supervised, talking over Unix sockets:

- `dlby-super`: starts every child, restarts crashes and hangs (heartbeats)
- `cap-radar` / `cap-radar-left`: read and decode each side's LD2451 frames
- `cap-sonar`: drives the HC-SR04 with kernel-timestamped GPIO edges
- `fuse-hub`: per-sensor health (OK / DEGRADED / BLIND) and fusion
- `store`: crash-consistent append-only log (CRC records, fsync policy)

Both sides get the radar's early APPROACHING warning; the side with the
ultrasonic also gets the confirmed IN_ZONE warning. The product makes no
network calls, and all detection logic is our own code. Design and failure
modes: `DESIGN.md`. Problem statement: `PROBLEM.md`.

## Build and test
```sh
make            # build
make test       # unit + end-to-end tests
make asan       # same tests under AddressSanitizer/UBSan
```
Needs gcc, make and Linux (GPIO and UART APIs), so build on the Pi, or in a
Linux container on a Mac.

## Run
```sh
export DLBY_RUN_DIR=$HOME/dlby-demo/run DLBY_LOG_DIR=$HOME/dlby-demo/log DLBY_BIN_DIR=$HOME/dlby-demo/bin
build/dlby-super                         # starts all children
build/tools/dlby-logcat ~/dlby-demo/log  # summarise the stored log
```
Step-by-step demo: `docs/DEMO.md`. Installed as a systemd service with
`sudo make install` (`deploy/dlby.service`).

## Status
- Done: capture for both radars and the ultrasonic, radar target decoding, supervisor, store, sensor health
- In progress: fusion state machine (Jackson), mirror LED / buzzer alert and status LCD, 48 h soak
- Evidence from the real hardware: `docs/evidence/`
