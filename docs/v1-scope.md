# MFS_1 v1 Scope and Configuration Roadmap

Project CLASS — Multi-Function Sensor 1. Recorded 2026-08-17.

The first build is deliberately much simpler than `../alc_drawer_master`. This
records what v1 does, what is explicitly deferred, and the configuration model to
design toward so that later work does not require rework.

## 1. v1 functional requirement

1. **Start up.** Cold start detected via nPM2100 reset reason (`ColdPowerUp`).
2. **Set date/time on first start** from a trusted provisioner
   (`docs/power-budget.md` §8.7). Monotonic floor and bounded-forward-jump rules
   apply.
3. **ADXL367 wakes the SoC, not the PMIC** — INT1 → SoC GPIO. **INT2 must be
   left undriven; see §2.** Referenced activity, loop mode, 5 s inactivity time.
4. **Track arm state:** Active or Inactive. **Cold start defaults to Inactive.**
5. **Engineer's only action:** toggle Active/Inactive.
6. **LEDs:**
   - LED A on while **Inactive**.
   - LED B on while **Active and triggered**; extinguishes 5 s after the trigger,
     that 5 s being the ADXL367 loop period rather than a software timer.

Also required, because it is how the toggle arrives: the 100 ms / 6 s passive scan
loop and TAN validation (`docs/tan-scheme.md`).

### 1.1 Explicitly deferred

No alarm transmission, no nightly status, no fuel gauge reporting, no FEM, no
Coded PHY fallback, no configuration beyond the single toggle. The BLE **connection**
is also deferred — see §4.

## 2. Hardware hazard — ADXL367 INT2 and PMIC SHPHLD

`alc_drawer_master` routes ADXL367 INT2 → nPM2100 SHPHLD as its Hibernate wake
path. **MFS_1 does not use that path** (the SoC stays in System ON idle and takes
INT1 directly), and leaving INT2 driven is dangerous on two counts.

**Unintended Ship mode.** Per the nPM2100 datasheet: *"Press the button for a
duration of t<sub>PWROFF</sub>. The chip enters Ship mode when the button is
released (SHPHLD returns high)."* A 5 s INT2 assertion followed by release is
exactly that gesture. The v1 loop period would therefore **power the device off**,
recoverable only by a physical SHPHLD press.

**Absolute-maximum voltage.** From Nordic's nPM2100 FAQ: *"Can I drive the SHPHLD
pin with the SoC GPIO pin? **No.** … SHPHLD pin has an absolute maximum voltage of
1.9 V, so controlling it with, say, a 3 V logic (of the GPIO pin) can cause damage
to the device."* `alc_drawer_master` is within spec only because its ADXL367 sits
on the 1.8 V LSOUT rail (`M_LSOUT_MILLIVOLTS = 1800`). **If the ADXL367 were ever
powered from VOUT** — ~3.0 V once the boost is in pass-through, which is the normal
state on a CR123A — INT2 would present 3 V to that pin and damage the PMIC.

**Required mitigations, both:**

- Configure the ADXL367 so **INT2 is unmapped and never asserted**.
- Set `shiphold-longpress = "disable"` on the `nordic,npm2100` devicetree node,
  which disables the power-off function of the pin outright.
- Keep the ADXL367 on the **1.8 V LSOUT rail** regardless.

### 2.1 LDOSW should be forced Ultra-Low Power

LDOSW in `Auto` follows the device mode — Active mode gives High Power. Since MFS_1
now stays in Active mode permanently (never Hibernate), Auto would hold LDOSW in
High Power for the device's whole life. Force **Ultra-Low Power**: it still supplies
up to 2 mA, against an ADXL367 drawing ~1 µA.

The 0.3 µA PMIC quiescent assumed in `docs/power-budget.md` §3 presumes this.
Measure LDOSW quiescent during bring-up and correct the budget if it differs.

## 3. ADXL367 configuration traps

Both inherited from `alc_flush_master`, which documents them for the ADXL362.
**Verify each on the ADXL367 during bring-up rather than assuming they carry over.**

**Referenced mode, never Absolute.** The activity engine ORs all three axes. In
Absolute mode the ~1000 mg vertical gravity component permanently exceeds any
practical threshold, so the part latches awake at boot and never clears.

**Loop-mode boot behaviour.** In Loop mode the engine boots with `AWAKE = 1` and
inactivity detection enabled; activity detection is gated until inactivity fires
once. Configuring the 5 s production inactivity time at boot would therefore leave
the engine in inactivity-detection for 5 s before any motion could register. Write a
short inactivity time at startup so it resolves into activity-detection within a
second, then raise it to the production value.

Using the ADXL's loop timing as the LED B timeout is deliberate — the hardware does
the timing, so no software `k_timer` is needed. Same approach as Flush Master.

## 4. v1 needs no BLE connection at all

The eventual configuration flow is a command string preceded by a TAN, which needs a
GATT connection. **v1's only command is a single toggle bit**, and it fits in the
advertising payload alongside the TAN:

| Field | Bytes |
|---|---|
| Version | 1 |
| TAN (6 digits) | 3 |
| Command (toggle) | 1 |
| **Total** | **5** — inside the 12-byte ceiling (`docs/power-budget.md` §8.7.4) |

So v1 is **advert-only**: no GATT, no connection, no central role. The device scans,
validates a TAN, applies the command, and never transmits. This removes the entire
connection stack from the first build.

**Trade-off:** no acknowledgement to the phone. The LEDs are the feedback channel,
which is adequate on the bench and arguably preferable there. The outward connection
arrives in v2 with the configuration string.

Security is unaffected — the TAN authenticates the command, and single-use means a
captured advert cannot be replayed.

## 5. LED power — bench only as specified

An LED at 2 mA is **29× the entire operating budget** (`docs/power-budget.md` §3),
giving ~29 days of life. Even 0.5 mA gives ~106 days. A continuously-lit state LED
is therefore incompatible with battery operation.

For v1 bench work this is accepted. Gate it behind a Kconfig as
`alc_drawer_master` does with `CONFIG_DRAWER_DEBUG_LED` (bench `y`, production `n`).

**Production options, to decide later:**

| Option | Standing cost |
|---|---|
| LED on for a few seconds after a toggle only | zero |
| LED only while an engineer session is live | zero |
| Blink 10 ms every 5 s | ~4 µA |
| Continuous (as specified for v1) | ~2 mA — not viable |

## 6. Arm-state persistence — decision needed

**As specified, cold start defaults to Inactive.** This is fail-safe in the sense
that a serviced device does not come up armed.

The consequence to be aware of: a **brown-out would silently disarm** the device.
For a covert alarm sensor that is a silent loss of function, with no indication to
the operator. Options:

- **As specified** — cold start always Inactive. Simple; accepts silent disarm.
- **Persist arm state to NVS** and restore it, treating only a first-ever boot as
  Inactive. Survives brown-out; a device being serviced may come up armed.
- Persist, restore, **and log the cold start** so the operator sees an unexplained
  power interruption as a tamper/fault signal.

Implemented as specified for v1. The third option is the likely production answer.

## 7. Configuration roadmap

The engineer's app presents sliders trading **latency against battery life**,
resolving to one of ten presets. Plan for substantially richer configuration later,
**including ADXL367 parameters** (threshold, ODR, activity/inactivity times, loop
behaviour), so the config format should be extensible from the outset — a
tag/length/value or versioned-struct scheme, not a fixed positional record.

### 7.1 Preset ladder — cadence at a fixed 100 ms scan window

Derived from `docs/power-budget.md` §3: per-wake cost 393 µC (380 µC scan +
13 µC overhead), always-on floor 3.8 µA, 1450 mAh usable.

| Preset | Cadence | Average | CR123A life |
|---|---|---|---|
| 1 | 1 s | 397 µA | 5 months |
| 2 | 2 s | 200 µA | 10 months |
| 3 | 3 s | 135 µA | 1.2 years |
| 4 | 4 s | 102 µA | 1.6 years |
| 5 | **6 s** | **69 µA** | **2.4 years** — default |
| 6 | 8 s | 53 µA | 3.1 years |
| 7 | 10 s | 43 µA | 3.8 years |
| 8 | 15 s | 30 µA | 5.5 years |
| 9 | 20 s | 23 µA | 7.1 years |
| 10 | 30 s | 17 µA | 9.8 years |

### 7.2 Why the ladder stops at 30 s

A span of 1 s to 10 minutes was proposed. **Above ~30 s the cadence stops mattering:**

| Cadence | Average | Consumption-limited life |
|---|---|---|
| 30 s | 17 µA | 9.8 years |
| 60 s | 10 µA | 16 years |
| 5 min | 5.1 µA | 32 years |
| 10 min | 4.5 µA | 37 years |

The always-on floor of 3.8 µA alone would run 43 years, and a CR123A's shelf life is
around 10 years. Everything from 30 s upward is therefore **capped by the cell's own
chemistry, not by consumption** — the engineer would be trading real latency for a
number that cannot be delivered. Ending the ladder at 30 s keeps every step
meaningful and the app's battery-life readout honest.

### 7.3 Scan window is a second axis — fixed at 100 ms for now

The scan **window** trades detection reliability against power independently of
cadence, and should not be confused with the counterpart's 20–50 ms **advertising
interval** (`docs/power-budget.md` §6). A 100 ms window captures a 20 ms advertiser
with near-certainty; a 20 ms window would fall to roughly 80% per wake.

Fix the window at 100 ms and let the preset move cadence only. Exposing the window
as a second slider is possible later, but it trades reliability rather than latency
and needs its own detection-probability guidance in the app.
