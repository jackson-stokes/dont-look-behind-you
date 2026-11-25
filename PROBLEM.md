# Problem memo -- Don't Look Behind You (Jackson Stokes, Gage Boullosa)

## The user 
A person or place, named or nameable. Who will this sit next to? 

A driver in Fort Collins. The device mounts on the car: a sensor pod outside on each rear quarter, cabled to a Raspberry Pi 5 inside the car, with alert LEDs on the side mirrors like a factory blind-spot light.

## The problem 
What goes wrong, how often, and what it costs (money, worry, ruined batches, missed warnings). Observable, not hypothetical. 

A driver turning or changing lanes doesn't see a cyclist coming up in a blind spot and turns across them. The classic case is the "right-hook" (a right turn across a cyclist on the right); the left side has the same blind spot when the driver moves left across a cyclist. In Fort Collins, cyclists get hit by cars this way, costing hospital bills, legal fees, and insurance claims. Factory blind-spot monitoring is missing on older cars and is tuned to detect cars, not cyclists.

## Why a device 
The 3 a.m. test: why must something be physically present and always awake? Why doesn't a phone app already solve this? 

The sensors must be physically mounted outside the car, aimed at the blind spot, and awake the whole time the car is moving; neither sensor works through glass. A phone in the cabin has no rear-facing radar or ranging sensor and can't see the blind spot.

## The sensors 
Which two (or more), and how they COOPERATE (fused, correlated, or one pipeline) rather than merely coexist. 

- Two HLK-LD2451 24GHz radars (UART, ~10Hz), one facing rearward along each side of the car: range, speed, and approach direction at roughly 5-50m.
- One HC-SR04 ultrasonic (~16Hz; JSN-SR04T for the outdoor build), facing sideways beside the right radar: confirms something is in the 0.2-4m blind-spot zone on that side.

They are fused by a state machine that hands off between the two range bands: CLEAR -> APPROACHING (radar sees an approaching target) -> IN_ZONE (ultrasonic detects something consistent with the radar's last track) -> CLEAR. With both healthy, radar alone can warn (APPROACHING) but IN_ZONE needs ultrasonic confirmation; if one is lost, the survivor warns in a degraded pattern. The side without an ultrasonic gets the radar's APPROACHING warning only.

## The mechanisms 
First guess at two items from the Section 3.2 menu, one sentence of justification each. Allowed to change by the design doc. 

C. Real-time scheduling (SCHED_FIFO/SCHED_RR) for a latency-critical path. 

A warning reaches the driver through three processes (sensor capture, fusion, alert), and under CPU or disk load each hand-off can be delayed by tens of milliseconds, so the whole alert chain needs SCHED_FIFO to keep that delay bounded.

E. A multi-process architecture: sensors isolated in separate processes, a hub aggregating over IPC (shared memory, message queues, or Unix domain sockets), a supervisor that survives any child dying.

One process per sensor plus a fusion hub under a supervisor, so an unplugged or crashed sensor degrades the system instead of taking it down.

(D, the storage layer, is a candidate third: ignition-off cuts power mid-write.)

## The risk 
The single thing most likely to sink this project. Name it now; it is cheaper to meet in Week 5 than in Week 14.

The radar may not detect cyclists far enough out. Its ~100m spec is for cars; a cyclist is a much smaller target. Go/no-go test: a cyclist passes the parked car, logging first-detect range over 5+ passes, with a target of at least 20m.
