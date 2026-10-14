# Problem memo -- Don't Look Behind You (Jackson Stokes, Gage Boullosa)

## The user 
A person or place, named or nameable. Who will this sit next to? 

A driver in Fort Collins. The device mounts on the car: a sensor pod outside on the right rear quarter, cabled through the door seal to a Raspberry Pi 5 inside the cabin, with an alert LED on the dash or right A-pillar.

## The problem 
What goes wrong, how often, and what it costs (money, worry, ruined batches, missed warnings). Observable, not hypothetical. 

A driver turning right doesn't see a cyclist coming up in the right-side blind spot and turns across them (the "right-hook"). In Fort Collins, cyclists get hit by cars this way, costing hospital bills, legal fees, and insurance claims. Factory blind-spot monitoring is missing on older cars and is tuned to detect cars, not cyclists.

## Why a device 
The 3 a.m. test: why must something be physically present and always awake? Why doesn't a phone app already solve this? 

The sensors must be physically mounted outside the car, aimed at the blind spot, and awake the whole time the car is moving; neither sensor works through glass. A phone in the cabin has no rear-facing radar or ranging sensor and can't see the blind spot.

## The sensors 
Which two (or more), and how they COOPERATE (fused, correlated, or one pipeline) rather than merely coexist. 

- HLK-LD2451 24GHz radar (UART, ~10Hz), facing rearward along the right side: range, speed, and approach direction at roughly 5-50m.
- HC-SR04 ultrasonic (~20Hz; JSN-SR04T for the outdoor build), facing sideways: confirms something is in the 0.2-4m blind-spot zone.

They are fused by a state machine that hands off between the two range bands: CLEAR -> APPROACHING (radar sees an approaching target) -> IN_ZONE (ultrasonic detects something consistent with the radar's last track) -> CLEAR. Neither sensor alone raises the alert.

## The mechanisms 
First guess at two items from the Section 3.2 menu, one sentence of justification each. Allowed to change by the design doc. 

C. Real-time scheduling (SCHED_FIFO/SCHED_RR) for a latency-critical path. 

Ultrasonic range comes from timing the echo pulse, and 1ms of timing jitter is about 17cm of range error, so the capture path needs SCHED_FIFO.

E. A multi-process architecture: sensors isolated in separate processes, a hub aggregating over IPC (shared memory, message queues, or Unix domain sockets), a supervisor that survives any child dying.

One process per sensor plus a fusion hub under a supervisor, so an unplugged or crashed sensor degrades the system instead of taking it down.

(D, the storage layer, is a candidate third: ignition-off cuts power mid-write.)

## The risk 
The single thing most likely to sink this project. Name it now; it is cheaper to meet in Week 5 than in Week 14.

The radar may not detect cyclists far enough out. Its ~100m spec is for cars; a cyclist is a much smaller target. Go/no-go test: a cyclist passes the parked car, logging first-detect range over 5+ passes, with a target of at least 20m.
