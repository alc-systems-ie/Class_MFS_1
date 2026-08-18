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

### 2.0 The nPM2100 boot monitor must be stopped at boot

**`App::initPmic()` calls `TimerStop()` within ~18 ms of boot. Do not remove it.**

The boot monitor *is* the nPM2100 TIMER block. Per the datasheet it "power cycles
the host System on Chip when the software fails to boot within t<sub>BOOT_TIMER</sub>",
starts automatically when the chip enters Active mode, and is stopped by
"activating the timer stop task in `TASKS_STOP`". Stopping it is the intended
"the host booted successfully" handshake, not a workaround. `alc_drawer_master`
does the same — its hand-off notes list it as lesson 5, "resets the host ~9 s
after boot unless firmware calls `TimerStop()` early; sticky, survives reflash."

Things that do **not** work, both tried:

- The sticky `BOOTMONSEL` / `BOOTMONEN` bits (`ConfigureBootMonitor()`) are a
  selector for future power cycles. They do not stop a monitor already running.
- `SYSGDENSTATUS` (0xE2) bit 0 reports *configuration*, not running state — "boot
  monitor is active unless SYSGDENSTATE=0". It stays set after a successful stop,
  so it must never be used as a pass/fail check.

Once stopped it **cannot be re-enabled over TWI** until the next power cycle, so
the device currently has no ongoing watchdog. The same TIMER block in
`WatchdogReset` / `WatchdogPwrCyc` mode, kicked from the main loop, would provide
one — worth considering for a covert alarm sensor. **Open.**

**Hardware alternative:** `SYSGDEN` is a pin, not a register. Grounded, the boot
monitor never starts; left unconnected (as on the Drawer Master board) it is
enabled. Worth a deliberate decision on the MFS_1 board revision.

**How it presents if missed:** the device runs for ~9 s and power-cycles, forever.
On the bench that looked like LEDs blinking on a ~6 s cycle, an arm state that
would not stick, motion detection that "worked but not cleanly", and RTT going
silent after boot as the viewer lost sync on every reset. The giveaway is
`GetResetReason()` returning `BootMonitor` instead of `ColdPowerUp` — which is
printed in the boot log on every boot.

### 2.1 LDOSW should be forced Ultra-Low Power

LDOSW in `Auto` follows the device mode — Active mode gives High Power. Since MFS_1
now stays in Active mode permanently (never Hibernate), Auto would hold LDOSW in
High Power for the device's whole life. Force **Ultra-Low Power**: it still supplies
up to 2 mA, against an ADXL367 drawing ~1 µA.

The 0.3 µA PMIC quiescent assumed in `docs/power-budget.md` §3 presumes this.
Measure LDOSW quiescent during bring-up and correct the budget if it differs.

## 3. ADXL367 configuration traps

**Read the real datasheet, not a summary.** The full ADXL367 datasheet and a
register quick reference are on disk at
`../alc_help_at_hand/docs/adxl367.pdf` and `.../ADXL367_QRGs/`. A summarised
markdown copy exists at `../../v3.1.0/alc_mailbox_monitor/adxl367_datasheet.md`;
it documents the registers but **omits the loop mode initialization routine**,
which is the whole ballgame. Four failed bring-up attempts came from working off
the summary — see §3.1.

**Referenced mode, never Absolute.** The activity engine ORs all three axes. In
Absolute mode the ~1000 mg vertical gravity component permanently exceeds any
practical threshold, so inactivity can never be satisfied.

**`ACT_INACT_CTL` (0x27) holds 2-bit fields, not single bits.** `[5:4]` LINKLOOP,
`[3:2]` INACT_EN, `[1:0]` ACT_EN, where `01` = Absolute and `11` = Referenced.
`alc_drawer_master` defines these as `M_ADXL_ACT_EN = BIT(0)` and
`M_ADXL_ACT_REF = BIT(1)`, which ORs to the right value by coincidence rather
than by construction.

**`STATUS` reset value is 0x40 — the part powers up with AWAKE already set.** A
reading of `0x40` therefore proves nothing about engine state; it may simply never
have changed.

**Thresholds are 13-bit**, split `H[12:6]` / `L[5:0] << 2`, at 0.25 mg/LSB on the
±2 g range. So 150 ≈ 37.5 mg.

**Write `FILTER_CTL` as `0x23`, not `0x03`.** `0x23` is the reset value: ±2 g,
100 Hz, and `I2C_HS` (bit 5) set as it powers up. Writing a bare ODR value clears
`I2C_HS`.

### 3.1 Loop mode initialization routine — REQUIRED

Referenced mode compares each sample against an internally held reference, and
that reference is only valid once the engine has completed a cycle. Configure the
real thresholds up front and the engine never cycles: gravity reads as a permanent
~940 mg deviation against a ~37 mg threshold, inactivity is never satisfied, and
**AWAKE stays asserted forever**. Observed on hardware as `STATUS` stuck at `0x40`
indefinitely on a motionless board with a provably correct configuration.

The datasheet publishes the fix. Follow it verbatim — the point is to force one
immediate loop cycle with deliberately absurd thresholds, then install the real
ones:

| Step | Action |
|------|--------|
| 1 | `THRESH_ACT` **below the noise floor** — 1 LSB |
| 2 | `TIME_ACT` = 0 |
| 3 | `THRESH_INACT` **above 1 g** — full scale, 0x1FFF |
| 4 | `TIME_INACT` = 0 |
| 5 | `ACT_INACT_CTL` = 0x3F (loop, both referenced) |
| 6 | Other settings — `FILTER_CTL`, `INTMAP1/2` |
| 7 | `POWER_CTL` = **0x07** (measurement + autosleep) |
| 8 | **Wait for AWAKE to go LOW** — ~100 ms + 1/ODR. Measured at 115 ms |
| 9 | Install the real thresholds and timers, registers 0x20–0x26 |

Two things worth noting about step 9. It writes those registers **in measurement
mode**, so the datasheet's general "configure registers 0x00–0x2C in standby"
guidance does not govern them. And **AUTOSLEEP at step 7 is not optional** — it is
what moves the part between measurement and wake-up mode as the loop cycles, and
AWAKE reports which of those states it is in.

Using the ADXL's loop timing as the LED B timeout is deliberate — the hardware does
the timing, so no software `k_timer` is needed.

### 3.2 Power-up sequencing

The ADXL367 sits on the PMIC's LSOUT rail, which introduces two requirements the
firmware must honour:

- **Allow ~20 ms after LSOUT rises before probing.** The part must load fuses and
  reach standby first. Probing 0.57 ms after enabling the rail fails. This only
  shows up on a genuine cold power-up: a warm reset leaves LSOUT already live from
  the previous run, so the race stays hidden through any amount of `west flash`
  testing.
- **Bring the rail up in High Power, not Ultra-Low Power.** The datasheet requires
  supply current above 250 µA during power-up for correct fuse loading. LDOSW drops
  to ULP once the part is configured, so the idle saving is kept.

**Power-cycle LSOUT at boot** (disable, ~50 ms discharge, enable). nPM2100
registers survive an SoC reset, so enabling an already-live rail is a no-op that
would leave the ADXL in whatever mode the previous run ended in. The datasheet also
recommends a full discharge to 0 V when power cycling.

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
