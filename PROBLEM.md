# Problem memo -- Don't Look Behind You (Jackson Stokes, Gage Boullosa)

## The user 
A person or place, named or nameable. Who will this sit next to? 

The device will sit on or near the handlebars on a bicycle or other mode of transportation.

## The problem 
What goes wrong, how often, and what it costs (money, worry, ruined batches, missed warnings). Observable, not hypothetical. 

Bikers don’t see a car or other cyclist coming up behind them. In Fort Collins, many cyclists get hit by cars costing a lot of money in legal fees, hospital bills, insurance claims, etc.

## Why a device 
The 3 a.m. test: why must something be physically present and always awake? Why doesn’t a phone app already solve this? 

The device must be physically present to detect activity in the blind spots of cyclists. A phone app could not solve this problem because it is not equipped with the correct motion detection sensors.

## The sensors 
Which two (or more), and how they COOPERATE (fused, correlated, or one pipeline) rather than merely coexist. 

A motion sensor would send feedback to some sort of indicator. A sound sensor could also send feedback to the indicator if a loud sound like a horn was made around you. 

## The mechanisms 
First guess at two items from the Section 3.2 menu, one sentence of justification each. Allowed to change by the design doc. 

C. Real-time scheduling (SCHED_FIFO/SCHED_RR) for a latency-critical path. 

We could measure how accurately and quickly the system picks up incoming cars. 

E. A multi-process architecture: sensors isolated in separate processes, a hub aggregating over IPC (shared memory, message queues, or Unix domain sockets), a supervisor that survives any child dying.

There would be 2 sensors (1 on each side), which would act independently of one another. 

## The risk 
The single thing most likely to sink this project. Name it now; it is cheaper to meet in Week 5 than in Week 14.

The sensors may not have enough range to indicate to the cyclist with enough time for them to make a decision. 
