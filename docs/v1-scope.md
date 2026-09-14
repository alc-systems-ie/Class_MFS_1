# MFS_1 v1 Scope and Configuration Roadmap

Project CLASS — Multi-Function Sensor 1. Recorded 2026-08-17.

The first build is deliberately much simpler than `../alc_drawer_master`. This
records what v1 does, what is explicitly deferred, and the configuration model to
design toward so that later work does not require rework.

## 1. v1 functional requirement

1. **Start up.** Cold start detected via nPM2100 reset reason (`ColdPowerUp`).
2. **Set date/time on every boot** from a trusted provisioner — the clock is
   invalid on every boot and is never resumed from NVS (`docs/tan-scheme.md` §7).
   Monotonic floor and bounded-forward-jump rules apply.
3. **ADXL367 wakes the SoC, not the PMIC** — INT1 → SoC GPIO. **INT2 must be
   left undriven; see §2.** Referenced activity, loop mode, 5 s inactivity time.
4. **Track arm state:** Active or Inactive. **Cold start defaults to Inactive.**
5. **Engineer's action is an authenticated command, not a toggle** — a single
   16-byte encrypted service UUID that asserts arm state and settings together
   (§4, `docs/tan-scheme.md`).
6. **LEDs:**
   - LED A **acknowledges accepted commands**
     (`docs/superpowers/specs/2026-09-12-app-control-design.md` §6.7) — a bounded
     pattern plays once per accepted command, rather than marking Inactive as a
     standing state.
   - LED B **shows detection in both arm states** — lit for about 5 s on every
     detection, confirming a real trigger while Active or simulating one during
     tuning while Inactive, that 5 s being the ADXL367 loop period rather than a
     software timer.

Also required, because it is how commands arrive: the 100 ms passive scan
loop (every 6 s as originally scoped; now every 5970 ms, and continuous during the
arming exit delay — `docs/superpowers/specs/2026-09-14-scan-reliability-amendment.md`)
and day-key command validation (`docs/tan-scheme.md`).

### 1.0 THE ARM BOOLEAN IS DEFINITIVE — architectural invariant

**The device tracks `m_arming.State()` (`ArmingSequence`, `src/arming_sequence.hpp`
— `Inactive`, `Arming` or `Active`). The accelerometer is only ever ANDed with it.**

This is not a style preference. In the product the output switches a voltage, so a
device that fires while deactivated is dangerous. The invariant must hold as
further hardware is added.

Enforced in code by a single derivation point:

```cpp
// App::updateOutputState() — the only place the two are combined.
m_output_active = (m_arming.State() == ArmState::Active) && m_engine.DetectionMet() && delayPermitsFiring();
```

and a single sanctioned read, `App::IsOutputActive()`.

**Rules for anything added later** — voltage switch, alarm report, event counter,
BLE notification:

- **Call `IsOutputActive()`.** Never read INT1, the AWAKE bit, `Adxl367::ReadAwake()`
  or any accelerometer state and act on it directly.
- **Never re-derive the condition** at the consumer. `OutputSwitch` is the worked
  example: it is driven as `m_output_switch.Set(m_output_active);` from the
  derivation point itself, never from a second call site that could disagree with
  it. LED B is **not** this example — it is a bench-only detection indicator
  (`ledB = m_detection_met;`), lit in both arm states for tuning, and is gated out
  of production builds.
- **If the condition must change, change `updateOutputState()`**, so every consumer
  moves together and none is left behind.

The accelerometer GPIO spec `s_adxl_int1` is file-scope `static` in `app.cpp`
precisely so no other translation unit can reach it.

### 1.0.1 Arming is EDGE-TRIGGERED — safety critical

> **Amended 2026-09-14 — disarmed test mode:**
> `docs/superpowers/specs/2026-09-14-disarmed-test-mode-amendment.md` supersedes
> this section where they disagree. The ADXL367 is no longer held in standby
> while disarmed — the detection engine runs continuously in both arm states,
> and the part is reconfigured afresh on every arm and on every test restart
> (a disarm, a Settings command, or boot), not only on activation.
>
> **Further amended 2026-09-14 (bench session 1) — arming sequence and
> fire-pin isolation:** `docs/superpowers/specs/2026-09-14-arming-sequence-amendment.md`
> further supersedes the table and identifiers below. There is now a third arm
> state, `ArmState::Arming`, between Inactive and Active: an accepted Arm starts
> a 10 s exit delay (LED A dark, LED B suppressed) before the restart below
> runs, and the fire pins are `GPIO_DISCONNECTED` at every other time, attached
> only as the last step of a successful arm. The state lives in
> `m_arming.State()` (`ArmingSequence`), set via `App::disarmDevice()` (which
> calls `m_arming.Disarm()`) and `m_arming.BeginArming()` / `m_arming.Service()`
> — not a bare `m_arm_state` member or a `setArmState()` call.

**A trigger that was already asserted when the device was armed must never fire.**

The ADXL367 AWAKE bit is a **level, not a latch**. Once motion occurs it stays
asserted for the whole inactivity period (5 s) and **cannot be cleared by reading
STATUS**. So a naive `armed && triggered` test fires the instant the device is
armed, in response to motion that happened *before* arming.

**This is the common case, not an edge case.** An engineer handling the device in
order to arm it is itself motion, so AWAKE is very often asserted at that moment.
Without the guard the device would fire on nearly every activation.

**In the product the trigger switches a voltage**, so a false fire on activation is
dangerous rather than merely untidy.

**Implementation (amended — see banner above): the accelerometer is reconfigured
afresh on every arm and on every test restart, never merely left running with a
stale level.** Rather than leave a continuously-running part and filter its stale
level, there is no stale level to inherit — the datasheet's loop mode
initialization routine soft-resets the part and forces one activity/inactivity
cycle, which drives AWAKE low (§3.1). The order is what makes this safe, and it is
deliberate in both directions:

| Activate (Arm) | Deactivate (Disarm) |
|---|---|
| 1. Accepted → `m_arming.State()` = `Arming`; fire pins stay isolated, nothing for 10 s | 1. Disable the fire pins (`OutputSwitch::Disable()`) |
| 2. Restart: `ConfigureLoopMode()` — soft reset, bootstrap cycle, real thresholds | 2. `m_arming.State()` = `Inactive` (cancel any arming) |
| 3. Confirm `AWAKE == 0` from STATUS | 3. `updateOutputState()` — output re-derives to 0 (pins already isolated) |
| 4. Enable the fire pins — the last step | 4. Restart: reconfigure the ADXL367 afresh, test resumes from zero |
| 5. `m_arming.State()` = `Active` only if every step succeeded, LED A plays Armed | |

Any arming-step failure fails safe: disable the fire pins, `Inactive`, restart
the test disarmed, raise the warning
(`docs/superpowers/specs/2026-09-14-arming-sequence-amendment.md` §4).

On deactivation the fire pins are disabled **before** the state moves to
`Inactive` — the pins going safe does not wait on, or depend on, the boolean at
all. The state then moves to `Inactive` and only then is the output re-derived,
because **the output is derived from the state, not stored beside it** (§1.0).
The re-derivation runs immediately after and always before the sensor is
touched, so there is no instant at which a deactivated device still reads as
triggered.

`Adxl367::Standby()` parks INTMAP1/INTMAP2 active-low with nothing mapped before
dropping POWER_CTL, so both pins idle HIGH — used between a counted activation and
its cooldown, not while merely disarmed. The part is configured at boot (cold
start is Inactive, but Inactive now means testing — see the amendment banner
above) and on every restart thereafter, so it no longer sits unconfigured from
boot until the first activation. The INT2/SHPHLD polarity bit (§2) must not depend
on how soon someone happens to arm the device, and reconfiguring on every restart
guarantees it does not.

`enableAccelerometer()` **refuses to arm** if the part will not configure or its
AWAKE state cannot be read. A device that reported itself armed with a dead sensor
would be a silent loss of function; instead it stays Inactive with LED A lit.

`m_ignore_stale_trigger` is kept as belt and braces: if the part is somehow awake
in the moments between configuring and arming, the flag suppresses it until INT1
de-asserts. It should not normally be set.

Configuring per-arm has a second benefit: the referenced-inactivity reference is
always captured **in the orientation the device is actually left in**, which is
the failure the full-scale `THRESH_INACT` also guards against (§3.1).

`alc_drawer_master` hits the same class of bug and solves it differently, because
it uses **latched** activity rather than loop-mode AWAKE: it clears the latch
immediately before arming, `adxl.ReadActivityLatched(discardLatch)`, with the
comment "a stale latch holds SHPHLD low -> instant false wake". **That fix does not
transfer** — clearing a latch has no effect on a level.

Any future consumer of this signal (alarm report, voltage switch, event counter)
must use the gated `triggered`, never the raw INT1 level.

### 1.1 Explicitly deferred

No alarm transmission, no nightly status, no fuel gauge reporting, no FEM, no
Coded PHY fallback. The BLE **connection** is also deferred — see §4.

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

The command is a single authenticated assertion of arm state and settings together
(§4 of `docs/superpowers/specs/2026-09-12-app-control-design.md`), carried in one
128-bit service UUID — no GATT, no connection, no central role needed. It is
**16 bytes**: rotating ID (4) + ciphertext (8) + tag (4), the same payload size as
the provisioner time sync (`docs/tan-scheme.md` §6.1).

So v1 is **advert-only**: the device scans, authenticates the command, applies it,
and never transmits. This removes the entire connection stack from the first build.

**Trade-off:** no acknowledgement to the phone over the radio — **LED A is the
acknowledgement**. It plays a bounded pattern once an accepted command has taken
effect (`docs/superpowers/specs/2026-09-12-app-control-design.md` §6.7), which is
adequate on the bench and arguably preferable there. An outward connection, if ever
needed, remains a later addition rather than something v1 is missing.

Security is unaffected — the command is authenticated under the day key, and the
rotating sequence number means a captured advert cannot be replayed.

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

**Answered for LED A** (`docs/superpowers/specs/2026-09-12-app-control-design.md`
§6.7): bounded patterns only, a few seconds per command — the first row above,
generalised from "after a toggle" to "after any accepted command". A production
build (`CONFIG_MFS_DEBUG_LED=n`) leaves LED A dark between patterns rather than
lighting it while Inactive, so the standing cost is zero. LED B remains a bench-only
detection indicator, never a production consumer, and is not addressed by this
answer.

## 6. Arm-state persistence — decided

**Arm state is not persisted.** Cold start always defaults to Inactive, including
after a brown-out — the fail-safe option below, as implemented.

The consequence is sharper than originally scoped: **a reset also invalidates the
clock** (`docs/tan-scheme.md` §7), so the device comes up not merely Inactive but
**unresponsive to commands** until a provisioner next syncs it. For a covert alarm
sensor that is a silent double loss of function, with no indication to the
operator until someone visits the device with the app. Options considered:

- **As specified** — cold start always Inactive. Simple; accepts silent disarm.
- **Persist arm state to NVS** and restore it, treating only a first-ever boot as
  Inactive. Survives brown-out; a device being serviced may come up armed.
- Persist, restore, **and log the cold start** so the operator sees an unexplained
  power interruption as a tamper/fault signal.

**Decided: none of these for v1 — the intended refinement is the nPM2100's
SCRATCHA register**
(`docs/superpowers/specs/2026-09-12-app-control-design.md` §7.1). SCRATCHA survives
exactly the resets NVS should not survive (brownout, watchdog, SoC reset) and
clears on the one event it should (battery removal), which none of the three
options above can express with NVS alone. It cannot hold the access state, and
restoring arm state from it while the clock stays invalid on every boot still
leaves the device waiting on a provisioner sync before it will obey a disarm — the
correct failure direction for an output that switches a voltage. Open for a later
task; both the arm-state and clock-offset uses are unimplemented.

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
| 5 | **6 s** | **69 µA** | **2.4 years** — default at the time this ladder was written |
| 6 | 8 s | 53 µA | 3.1 years |
| 7 | 10 s | 43 µA | 3.8 years |
| 8 | 15 s | 30 µA | 5.5 years |
| 9 | 20 s | 23 µA | 7.1 years |
| 10 | 30 s | 17 µA | 9.8 years |

**Amended 2026-09-14:** the actual default is now **5970 ms** (5.970 s), not
preset 5's 6000 ms — 6000 ms is exactly 32 × 187.5 ms, a Mac's measured
advertising interval, so every scan landed at the same phase of its cycle and
whole commands were missed. (An intermediate value, 5906 ms, was tried first
against a closed-form drift rule that review later proved unsound; a
brute-force phase-coverage simulation showed it still misses 35 % of commands
against Apple's recommended 211.25 ms interval. A later value, 5876 ms, passed
the simulation but sat only 3 BLE units from its passing island's edge; the
owner moved the default to 5970 ms, the middle of the wider 9537–9565 island.)
Average current at 5970 ms recomputes to **~68.8 µA**, still ~2.4 years on a
CR123A. See
`docs/superpowers/specs/2026-09-14-scan-reliability-amendment.md` and
`docs/power-budget.md` §3. The ladder above is otherwise unaffected — it is
kept as the derivation record for the other nine cadences, none of which
changed.

**The ladder's cadences are NOT all usable as periods.** Presets 3 (3 s), 8 (15 s)
and 10 (30 s) are exact multiples of the Mac's measured 187.5 ms (16×, 80× and 160×),
so their per-scan phase drift is zero and they **fail the phase-coverage simulation**
in the amendment §2 exactly as 6000 ms (32×) did: a command starting at a bad phase is
missed by every scan. "Not a near-multiple" is necessary but not sufficient — check any
candidate period against every measured interval with `tools/scan_phase_check.py`
before adopting it, not by inspection; the withdrawn closed-form rule (d × N ≥ I − W)
must not be used for this. At 15 s and above a 30 s command holds only one or two
scans, and passing the simulation is hard to achieve at all.

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
cadence, and should not be confused with the counterpart's advertising **interval**
(`docs/power-budget.md` §6). A 100 ms window captures a 20 ms advertiser with
near-certainty; a 20 ms window would fall to roughly 80% per wake.

**Amendment, 2026-09-13:** the 20–50 ms interval above was a design assumption, not
a measurement. A phone advertises at ~187 ms (macOS measurement; an iPhone
re-measure is pending, `docs/superpowers/plans/2026-09-13-bench-checklist.md` §7),
and the interval is not ours to set — iOS/macOS do not expose it. The app
compensates with a 30 s advertising window instead
(`docs/superpowers/specs/2026-09-12-app-control-design.md` §3). The scan window
stays fixed at 100 ms regardless; only the counterpart's cooperation assumed below
has changed.

Fix the window at 100 ms and let the preset move cadence only. Exposing the window
as a second slider is possible later, but it trades reliability rather than latency
and needs its own detection-probability guidance in the app.
