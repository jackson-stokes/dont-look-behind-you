# Hardware plan

Status as of 2026-10-12. Feeds the M2 design document.

## Direction
- **Platform:** car-mounted. The bike was too tight for size and power, and the handout (§2) requires a Raspberry Pi, so the product runs on the **Pi 5**. The ESP32 is a bench tool only.
- **Moment:** while driving, a cyclist comes up in the right-side blind spot just before a right turn (the "right-hook").
- **Sensors (fused, §3.1):**
  - **HLK-LD2451 24GHz radar** (UART, ~10Hz): rearward along the right side; range, speed and approach direction at roughly 5–50m+.
  - **HC-SR04 ultrasonic** (trigger/echo, 16Hz: needs ≥60ms between pings): sideways out of the right rear quarter; confirms something is in the 0.2–4m blind-spot zone.
  - **Fusion:** a state machine hands off between the two range bands, CLEAR → APPROACHING (radar) → IN_ZONE (ultrasonic, consistent with the radar's last track) → CLEAR. With both healthy, radar alone can warn (APPROACHING) but IN_ZONE needs ultrasonic confirmation; if one is lost, the survivor warns in a degraded pattern.
- **Mechanisms (§3.2):**
  - **C (SCHED_FIFO):** bounds alert latency across capture → fusion → alert under load. Range accuracy doesn't need it, because echo edges are kernel-timestamped (see DESIGN.md §2).
  - **E (multi-process + supervisor):** one process per sensor plus a fusion hub, so unplugging a sensor degrades the system instead of crashing it.
  - **D (storage layer):** a natural fit, since ignition-off cuts power mid-write.

## Inventory
| Item | Role |
|---|---|
| Raspberry Pi 5 (SANOOV case) | Product compute, inside the cabin |
| HLK-LD2451 radar | Long-range sensor (arrived) |
| HC-SR04 (Temu kit) | Short-range sensor. Bench now; swap to a waterproof JSN-SR04T for the outdoor pod |
| 1602 LCD + I2C backpack | Status display for bench and demo. Not for the driver while moving |
| ESP32 DevKit (ESP-WROOM-32) | Bench testing only (`tools/bench/`) |
| Breadboards, HW-131 PSU, jumpers, resistors, LEDs | Prototyping, echo voltage divider |
| 18650 battery shield | Spare; not needed in a car |

## Still to buy
| Part | Why | ~$ |
|---|---|---|
| JSN-SR04T waterproof ultrasonic | Same protocol as the HC-SR04, survives outdoors | 8 |
| USB-C PD car charger, 5V/3A or more, + cable | Powers the Pi 5 from the 12V socket | 15 |
| Weatherproof project box + magnet mount / VHB tape | Exterior sensor pod | 10 |
| ~2m shielded 4-core cable, connectors, heat shrink | Pod-to-cabin run through the door seal | 10 |
| Active buzzer, 2N2222 | Audible driver alert (check the Temu kit first) | 0–5 |

## Physical layout (car)
- **Sensor pod**, outside on the right rear quarter:
  - Radar faces rearward, angled 10–20° outward.
  - Ultrasonic faces straight out sideways at about a cyclist's hip height.
  - Neither sensor works through glass.
- **Cable** (~1.5–2m) through the rear door seal to the **Pi 5 on the rear seat or floor**.
- **Alert:** LED on the dash or right A-pillar, plus a buzzer.
- **Power:** a USB-C car charger. At 3A the Pi 5 caps current to its USB ports, which doesn't matter for header-wired sensors.

## Pi 5 pin map (draft; verify before wiring, power off)
| Function | Interface | GPIO | Notes |
|---|---|---|---|
| Radar | UART2 | TX 4 / RX 5 | `dtoverlay=uart2-pi5` |
| Ultrasonic TRIG | GPIO out | 23 | |
| Ultrasonic ECHO | GPIO in | 24 | 5V output: 1k/2k divider required |
| LCD | I2C1 | SDA 2 / SCL 3 | `dtparam=i2c_arm=on` |
| Alert LED | GPIO out | 17 | |
| Buzzer (via 2N2222) | GPIO out | 22 | |

## ESP32 bench test
Sketch: `tools/bench/ld2451_hcsr04_bench/ld2451_hcsr04_bench.ino`

| Part pin | ESP32 board label |
|---|---|
| LD2451 VCC / GND | VIN (5V, confirm on datasheet) / GND |
| LD2451 TX / RX | RX2 / TX2 (crossed) |
| HC-SR04 VCC / GND | VIN / GND |
| HC-SR04 TRIG | D18 |
| HC-SR04 ECHO | 1k → D19, 2k from D19 → GND (meter ≤3.3V first) |
| LEDs: radar link / radar target / sonar near | D25 / D26 / D27, each via 220–330Ω to GND |

**Flashing from Windows (Arduino IDE):**
1. In Boards Manager, install **esp32 by Espressif** (built and tested against core 3.3.11).
2. Select board **ESP32 Dev Module** and the board's COM port. If no COM port appears, install the CP210x or CH340 driver to match the chip next to the USB port, and use a USB cable that carries data.
3. Upload. If it stalls at `Connecting...`, hold **BOOT** until the upload percentage starts.
4. Open the serial monitor at **115200**.

**Expected output:**
- `SONAR <ms> <cm>`: values track a tape measure, and D27 lights under 100cm.
- `RADAR <ms> <len> <hex>`: D25 blinks per frame.
- D26 (target) depends on an **unverified** frame offset. Confirm it against the datasheet and real captures before trusting it.

## Risks
1. **Radar range on cyclists:** the ~100m spec is for cars. Go/no-go test: a cyclist passes the parked car; log first-detect range over 5+ passes, with a target of ≥20m.
2. **Ultrasonic outdoors while moving:** wind, rain, road noise and a narrow beam. Expect dropouts and handle them in fusion.
3. **The car's own motion:** filter on approaching targets.
4. **Testing safely:** parked first, then slow passes in an empty lot. The driver never reads the LCD while moving.
5. **"Why not factory blind-spot monitoring?":** argue for older cars, and for factory systems that are tuned to cars and miss cyclists.
