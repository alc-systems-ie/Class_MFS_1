# MFS_1 Power Budget and Sleep Architecture

Project CLASS — Multi-Function Sensor 1. Recorded 2026-08-17.

Energy budget for the periodic-scan duty cycle, the sleep architecture decision
behind it, and the constraints that fell out of the nPM2100 datasheet. All
component figures are cited; every derived number below can be recomputed from
them.

## 1. Decisions taken

| Decision | Value | Rationale |
|----------|-------|-----------|
| Sleep architecture | **System ON idle + RTC wake** | Not nPM2100 Hibernate — see §4 |
| Scan period | **6 s** | `CONFIG_MFS_SCAN_PERIOD_MS=6000` |
| Scan window | **100 ms** passive | 1.667% RX duty cycle; detection margin per §6 |
| ADXL367 | Continuous measurement mode | Always-on motion detect, 100 Hz ODR |
| nRF21540 FEM | **Not fitted in this version** | Costs 3 dB TX; saves BOM and risk — see §5 |
| Battery | CR123A (3.0 V Li-MnO2) | ~1450 mAh usable behind the nPM2100 |
| TANs | **Day-indexed, 10/day, expire at day end** | Per-day paper sheet; lost sheet compromises one day only — §8.1 |
| TAN persistent state | **4 bytes** | Horizon is unbounded, not 1 or 3 years — §8.2 |
| Day boundary | **04:00 UTC**, no multi-day window | Keeps clock error out of working hours — §8.5 |
| Timekeeping | **LFXO** — Abracon ABS06N, CL 9 pF | ±20–50 ppm; load cap set, trim pending — §8.5.3 |
| Expected life | **~2.4 years** | Bracket 2.2–2.7 years, §3 |

## 2. Component reference figures

| Parameter | Value | Source |
|-----------|-------|--------|
| nRF54L05 radio RX, LE 1M, HFXO | 3.4 mA typ @ 3 V | nRF54L15/L10/L05 Datasheet, I<sub>RADIO_RX0</sub> |
| nRF54L05 radio RX, measured (`scan_adv`) | 3.73 mA | Nordic DevZone — see erratum note §7 |
| nRF54L05 CPU, 128 MHz from NVM | 2.6 mA @ 3 V | Datasheet, CPU processing |
| nRF54L05 System ON idle | 0.7–1.7 µA | Datasheet, depends on retention config |
| nPM2100 I<sub>QHIB</sub> (Hibernate, timer, LDOSW off) | 320 nA | nPM2100 Datasheet, system electrical spec |
| nPM2100 I<sub>QHIB_PT</sub> (Hibernate_PT, timer) | 175 nA | nPM2100 Datasheet |
| nPM2100 I<sub>QPT</sub> (pass-through quiescent) | 170 nA | nPM2100 Datasheet |
| nPM2100 I<sub>QULP</sub> (boost ultra-low-power) | 300 nA | nPM2100 Datasheet |
| nPM2100 LDOSW, Ultra-Low Power mode | up to 2 mA out | nPM2100 Datasheet, LDOSW |
| ADXL367 measurement mode, 100 Hz ODR | 0.89 µA @ 2.0 V | ADXL367 Datasheet, supply current |
| ADXL367 motion-triggered wake-up mode | 181 nA | ADXL367 Datasheet |
| ADXL367 standby | 40 nA | ADXL367 Datasheet |
| nRF21540 RX (LNA active) — *not fitted, §5* | ~5 mA | nRF21540 Product Specification |
| nRF21540 standby — *not fitted, §5* | ~0.4 µA | nRF21540 Product Specification |
| nRF54L05 max TX power (QFN) | +7 dBm | nRF54L05 product page |
| nRF54L05 RX sensitivity, LE 1M | −96 dBm | nRF54L05 product page |

The nPM2100 operates down to 0.7 V V<sub>BAT</sub>, so essentially the whole
CR123A discharge curve is usable. 1450 mAh is taken as usable capacity against a
1500 mAh nominal cell, allowing for self-discharge over the service life.

## 3. The budget

System ON idle, 100 ms passive scan every 6 s. Duty cycle 100/6000 = 1.667%.

| Contributor | Basis | Average current |
|-------------|-------|-----------------|
| Passive scan RX | 3.8 mA × 1.667% | **63.3 µA** |
| Scan start/stop CPU + HFXO ramp | 2.6 mA × ~5 ms / 6 s | 2.2 µA |
| nRF54L05 System ON idle (RAM retention + RTC) | continuous | 2.5 µA |
| ADXL367, autosleep in wake-up mode while still | continuous | 0.2 µA |
| nPM2100 quiescent (pass-through / ULP) | continuous | 0.3 µA |
| **Total** | | **≈ 68 µA** |

**1450 mAh ÷ 0.0685 mA = 21,170 h ≈ 882 days ≈ 2.4 years.**

The ADXL367 line is 0.2 µA rather than the 0.89 µA of measurement mode because
**AUTOSLEEP** drops the part into wake-up mode (~180 nA) whenever it is still, and
it is still almost all the time. That is required for the loop engine to work at
all (`docs/v1-scope.md` §3.1), so the saving is a side effect rather than a
choice. It is worth about 0.8 µA — roughly 1%, or ten days across the service
life — so it changes no decision.

### MEASURED 2026-08-18 — 75 µA at 3.0 V

Power Profiler Kit II in Source Meter mode at 3000 mV, LED A disabled
(`CONFIG_MFS_BLINK_MS=0`), debugger detached, 1-minute averaging window:

| | Predicted | Measured | Delta |
|---|---|---|---|
| Average current | 69 µA | **75 µA** | +8.7% |
| CR123A life | 2.4 years | **~2.2 years** | −8% |

**The model is good to within 9%**, built from datasheets before hardware existed.

A shorter averaging window earlier read 88 µA. That is believed to be settling or
handling: any disturbance holds the ADXL367 in measurement mode (~0.89 µA) instead
of autosleep wake-up mode (~180 nA) for 5 s, and a brief window catches it. Let
the board sit untouched for ten minutes before trusting an average.

**The fresh-cell figure is not the life figure.** See §3.1 — consumption rises as
V<sub>BAT</sub> falls, so a single 3.0 V reading understates lifetime consumption.

### 3.1 OPEN — consumption versus supply voltage

**The BOOST output voltage is never configured** (`App::initPmic()` sets LDOSW only),
so VOUT is whatever the VSET pin selects: 3.0 V unconnected, 1.8 V grounded.

This matters because BOOST enters pass-through only "when battery voltage is at
least 100 mV above the target VOUT". If VOUT is 3.0 V, a CR123A drops below 3.1 V
almost immediately and the boost is therefore **switching for essentially the whole
service life**, not just the tail — which contradicts the assumption in §10 that
battery current ≈ load current.

To resolve, sweep the PPK2 and record:

| Supply | Emulates | Measured | Notes |
|---|---|---|---|
| 3.2 V | Fresh, above pass-through threshold | | If markedly lower than 3.0 V, VOUT is 3.0 V and the boost was switching |
| 3.0 V | Fresh CR123A | **75 µA** | measured |
| 2.5 V | Mid-life | | |
| 2.0 V | Near end of life | | |

**If it is confirmed, there is a design win available:** setting `BOOST.VOUT` to
1.8 V via `BoostSetVoltage()` would hold pass-through down to ~1.9 V V<sub>BAT</sub>,
i.e. nearly the whole discharge curve, and the nRF54L05 runs from 1.7 V. One call
in `initPmic()`.

A proper life estimate integrates consumption across the discharge curve rather
than dividing capacity by the fresh-cell figure, so the 2.5 V and 2.0 V points
matter more than the 3.0 V one.

Bracket on the soft numbers:

| Case | Assumptions | Average | Life |
|------|-------------|---------|------|
| Best | Datasheet-typ throughout: RX 3.4 mA, idle 1.7 µA, ADXL 0.89 µA, PMIC 0.17 µA | 62 µA | 2.7 years |
| Planning | As tabulated above | 69 µA | **2.4 years** |
| Worst | Measured RX 4.0 mA, idle 3.0 µA, 8 ms scan overhead | 75 µA | 2.2 years |

**The scan is ~91% of the budget.** Trimming the ADXL ODR and tuning RAM
retention together move the total by under 2 µA. Duty cycle is the only lever
with real authority — dropping the FEM entirely (§5) bought 0.4 µA, about five
days.

Routes to 3 years, if the requirement changes:

| Change | Average | Life |
|--------|---------|------|
| 100 ms every **8 s** | 53.0 µA | 3.1 years |
| **75 ms** every 6 s | 53.5 µA | 3.1 years |

## 4. Why System ON idle, not nPM2100 Hibernate

`alc_drawer_master` hibernates the PMIC between events and cold-boots the SoC on
each wake. That is correct at its 1800 s cooldown. It is the wrong tool at a 6 s
cadence, for two reasons.

**Hibernate saves almost nothing here.** In the 6 s budget above the entire
continuous sleep contribution is 4.2 µA of 70 µA — 6%. Hibernate could at best
reduce part of that by a few hundred nA.

**The cold boot it forces costs far more than it saves.** A PMIC wake plus SoC
cold boot plus `bt_enable()` is roughly 100 ms at ~2.5 mA — 250 µC per wake, or
**~42 µA averaged over a 6 s period**. That is 10× the entire sleep contribution
it was meant to reduce, and it would consume most of the budget before the radio
is switched on at all.

For the record, the originally proposed duty cycle (1 s scan every 5 s with
Hibernate cold-boot cycling) budgeted at **815 µA → ~74 days** on a CR123A. The
hard ceiling for that duty cycle is 89 days — 1450 mAh ÷ 0.68 mA, the radio alone
at 20% RX with a hypothetical zero-cost wake. No boot or sleep optimisation can
beat it; only the duty cycle could.

System ON idle also keeps the Global RTC running, which the TAN scheme needs
(§8).

### nPM2100 Hibernate constraint — for reference

Should Hibernate ever be reconsidered: **Hibernate_PT cannot power the ADXL367.**
Per the nPM2100 datasheet, Hibernate_PT sets BOOST to pass-through, **disables
LDOSW**, and executes a full power-up including register reset on wake. Only plain
Hibernate (320 nA, not 175 nA) can hold LDOSW on in Ultra-Low Power mode to keep
the ADXL367 supplied so it can assert INT2 → SHPHLD. `alc_drawer_master`
documents this same trap in its hand-off notes.

## 5. nRF21540 FEM — removed in this version

The FEM is **not fitted** on MFS_1. What that costs and saves:

**Battery: nothing meaningful.** It removes only the 0.4 µA standby — about five
days of the 872. The LNA was already specified off during the scan (below), so
there was no scan-path saving to make.

**Link budget: −3 dB on transmit.** The nRF54L05's native maximum is +7 dBm
(QFN) against the +10 dBm at antenna that `alc_drawer_master` achieves via
nRF21540 POUTB. Receive is unchanged at −96 dBm (LE 1M) sensitivity. Coded PHY
S=8 remains available and contributes far more (+12 dB coding gain vs LE 1M) than
the 3 dB lost, so the link stays comfortable — but any range figure inherited from
`alc_drawer_master` must be re-derated by 3 dB.

**Simplification, which is the real reason.** No MPSL FEM Kconfig, no `spi30`, no
TX_EN/RX_EN/PDN/MODE GPIOs, no `supply-voltage-mv` build trap, smaller BOM and
board. It also removes the standing risk recorded below.

### Why the LNA was never going to be used

Retained for the record, and because it applies to any future FEM-equipped
variant: the nRF21540 LNA draws ~5 mA when active. Enabled across the 100 ms scan
window it would add 5 mA × 1.667% = **83 µA** — more than doubling the total
budget and cutting life to roughly 10 months. Any FEM-equipped variant of this
design must keep the LNA off for the periodic scan and engage the FEM only for the
transmit/connect burst following a detected event, at POUTB (+10 dB) per the Irish
EIRP allocation.

## 6. Detection probability at a 100 ms window

A passive scanner parks on one primary channel per window. An advertiser
transmits on 37/38/39 within each advertising event, so the scanner gets one
opportunity per advertising event, and P(catch) ≈ min(1, W / T<sub>adv</sub>).

| Counterpart advertising interval | Events per 100 ms window | P(detect) per wake | Within 2 wakes (12 s) |
|---|---|---|---|
| 20 ms | 4–5 | effectively certain | certain |
| 50 ms | ~2 | >99% | certain |
| 100 ms | ~1 | ~95% | >99% |
| 152.5 ms | <1 | ~65% | ~88% |

**Specify 20–50 ms advertising interval on the hub/phone counterpart.** It is not
battery-constrained, so this is the cheap half of the trade. At 100 ms the design
does not *depend* on that cooperation the way a 60 ms window would have.

## 7. Erratum 20 — CONSTLAT before RX

nRF54L15 erratum 20's workaround requires CONSTLAT to be requested before
enabling RX. Omitting it is the documented cause of measured scan current landing
at 3.73 mA against the datasheet's 3.4 mA. Verify CONSTLAT handling during
bring-up before trusting any measured scan figure against this budget.

## 8. TANs — threat model, storage, timekeeping

### 8.1 The requirement

An engineer working on a device is issued a **paper TAN sheet for that day only**
— 10 TANs. If the sheet is lost, **only that day is compromised**; the sheet is
useless the following day. Expiry is by date, and it happens whether or not the
TANs were consumed.

This is the governing requirement for everything in this section. It has three
consequences that follow directly:

1. **TANs must be day-indexed.** Counter-indexed (iTAN/HOTP-style) TANs are
   ruled out — a counter-indexed sheet stays valid until consumed, which is
   exactly the property the requirement forbids.
2. **No multi-day validity window.** A ±1 day window would give a lost sheet three
   days of life and would let tomorrow's sheet work today. Ruled out.
3. **The device's own clock is the sole arbiter of expiry**, and is therefore a
   security component, not a convenience. See §8.4 for why the date cannot be
   taken from the presenter.

### 8.2 Storage collapses to 4 bytes

Because a TAN expires by date regardless of consumption, a consumed-flag for any
past day is dead weight — those TANs are already rejected on date grounds and can
never be replayed. **Only the current day's consumption needs tracking:**

| Item | Size |
|------|------|
| Current day index | 2 bytes (uint16 days since epoch, 179 years) |
| Consumed bitmap, current day only | 10 bits → 2 bytes |
| **Total persistent TAN state** | **4 bytes** |

TANs are derived, never stored: `TAN(day, n) = truncate(HMAC-SHA256(seed, day ‖ n))`,
computed on demand by the nRF54L05 CRACEN engine in hardware. Validating a
presented TAN is 10 HMAC operations against the current day.

Consequences worth noting:

- **The 1-year / 3-year TAN horizon is moot.** Nothing scales with the horizon.
  The device's TAN capability is unbounded; battery life (§3) is the only limit.
  The engineer-side tool generates any date's sheet from the seed.
- The 457-byte bitmap and the ~44 KB explicit-storage option previously considered
  are both obsolete. There is no NVS sizing or wear problem left to solve.
- 4 bytes of state rewritten a few times a day is trivial NVS traffic.

### 8.3 Guess rate

A 6-digit TAN is ~10<sup>6</sup> of code space; 10 valid codes on any given day
gives a **1 in 100,000 hit rate per guess**. Day-scoping is what makes this safe —
the same 10 TANs against an unexpired multi-year pool would be far weaker.

Add a **retry counter with lockout or exponential backoff**.
`alc_drawer_master`'s auth payload already carries a `tries` field to model on.
Note that the 6 s scan cadence caps an attacker's attempt rate anyway — an
unauthenticated peer cannot drive attempts faster than the device listens.

### 8.4 Why the date cannot come from the presenter

Tempting shortcut, worth closing off explicitly: let the engineer's phone present
`(date, TAN)` together and validate the TAN against the presented date. This
defeats expiry entirely — a holder of a lost day-N sheet simply presents date N.
The device must judge the date itself.

This has a structural consequence. TAN validation gates the connection, and time
sync happens over the connection, so **an engineer's visit cannot correct a device
whose clock is wrong** — they need a valid TAN to get in, and the device's own
(wrong) clock decides which TANs are valid. Hence §8.6.

### 8.5 Timekeeping — internal RTC, no external RTC IC

**Decision: no external RTC.** With the day boundary now a security boundary, the
reasoning changes from the earlier revision of this document, so it is set out in
full.

**Boundary drift is bounded by clock error, not by a day.** With no multi-day
window, clock error shows up as a small window either side of midnight in which
the wrong day's sheet is accepted (or the right one rejected):

| Clock source | Accuracy | Error per year | Notes |
|---|---|---|---|
| nRF54L LFRC | ±500 ppm | ±4.3 h | Declared `lfrc-accuracy-ppm = <500>`; no calibration driver — §8.5.1 |
| LFXO (32.768 kHz crystal) | ±20–50 ppm | ±10–26 min | **Preferred, subject to §8.5.2** |
| External RTC (RV-3028-C7) | ±1–2 ppm | ±1 min | Only worthwhile for the backup-cell property, §8.6 |

Drift accumulates from the **last time sync**, not from manufacture, so the figure
that matters is drift over the interval between engineer visits. At 500 ppm and
annual visits that is ±4.3 h, which pushes the 04:00 boundary as far as
23:45–08:15 — into working hours at the extremes. At LFXO accuracy it is tens of
minutes and never leaves the small hours.

#### 8.5.1 Correction — there is no LFRC calibration driver on nRF54L

An earlier revision of this document selected "HFXO-calibrated LFRC at ~±40 ppm"
on the basis that HFXO already runs every 6 s so calibration would be nearly free.
**That mechanism does not exist for this SoC family.** Zephyr's
`drivers/clock_control/nrf_clock_calibration.c` and its
`CONFIG_CLOCK_CONTROL_NRF_CALIBRATION` symbols are nRF52-era. nRF54L uses the
`nordic,nrf-lfclk` binding, which simply *declares* `lfrc-accuracy-ppm = <500>`
and offers no HFXO-referenced trimming of the RC oscillator.

So the options are genuinely: **500 ppm, an LFXO, or write our own calibration.**
Self-calibration remains feasible — measuring the RTC against HFXO on each scan
wake would give repeated free samples, and because it samples continuously it
would track temperature drift, which is the dominant term. But it is our code to
write, test and justify, not a Kconfig line.

#### 8.5.2 LFXO is fitted — selected, but the load capacitance needs trimming

**Confirmed 2026-08-17: the bespoke board fits a 32.768 kHz crystal.** LFXO is
therefore the timebase, giving ±20–50 ppm — tens of minutes per year, comfortably
inside the small hours around a 04:00 boundary. Self-written LFRC calibration
(§8.5.1) is not needed.

No Kconfig work is required. The resolved configuration for this target already
selects it:

```
CONFIG_CLOCK_CONTROL_NRF_K32SRC_XTAL=y      # LFXO is the LF clock source
CONFIG_CLOCK_CONTROL_NRF_ACCURACY=50        # stack assumes 50 ppm
CONFIG_NRF_GRTC_TIMER_SOURCE_LFXO=y         # GRTC clocked from LFXO
```

The GRTC is the nRF54L's always-on real-time counter, so the timekeeping
infrastructure is correct as it stands.

#### 8.5.3 The crystal, and the load capacitance that was wrong

**Crystal: Abracon ABS06N-32.768kHz-9-T.** The `-9-` field is the specified load
capacitance, so **CL = 9 pF**. Confirm the frequency tolerance grade from the
Abracon datasheet; the ABS06 family is typically ±20 ppm.

**What the devicetree property means.** It is the crystal's load capacitance in
femtofarads, not a raw register value. Per the nRF54L15 PS, quoted in
`zephyr/soc/nordic/nrf54l/soc.c`:

```
CAPVALUE = round((2*CAPACITANCE - 12) * (SLOPE + 0.765625*2^9)/2^9 + OFFSET/2^6)
  where CAPACITANCE is the desired capacitor value in pF, 4-18 pF in 0.5 pF steps
```

The `-12` is the device subtracting its own pin/package contribution, and
`FICR->XOSC32KTRIM` slope/offset apply the per-die factory trim. For HFXO the PS
wording is more explicit still — "the desired **total load capacitance**".

**The inherited value was wrong by 8 pF.** The DK board files set
`load-capacitance-femtofarad = <17000>`, and `alc_drawer_master` never overrides
it, so every build on this board silently inherited the **DK's** crystal
requirement. Against a 9 pF part that over-capacitates by 8 pF, pulling the
oscillator roughly **45–50 ppm slow** — around 25 minutes lost per year, on top of
the crystal's own tolerance. (Estimate from
Δf/f = (C1/2)·[1/(C0+CL_spec) − 1/(C0+CL_actual)] with ABS06N-typical C0 ≈ 1.2 pF,
C1 ≈ 2.2 fF; the coefficient depends on the crystal's motional and shunt
capacitance, so treat it as an order-of-magnitude figure.)

Now set to `<9000>` and stated explicitly in the MFS_1 overlay rather than
inherited.

**Why this went unnoticed, and why it will not announce itself:** Bluetooth LE
tolerates up to 500 ppm of sleep-clock accuracy. A crystal mispulled to 100 ppm
works normally — connections form, Coded PHY links hold, nothing logs an error.
Only accumulated drift reveals it, and `alc_drawer_master` keeps no clock, so it
had nothing to reveal.

**STILL OPEN — trim by measurement.** 9000 fF is the nominal value from the part
number, not a measured one. Board stray capacitance shifts the optimum: note that
`&hfxo` sits at 14000 fF against a nominally 8 pF crystal after trimming on this
same board. Trim the same way `&hfxo` was trimmed to +1.27 ppm residual. Two
practical methods:

- Divide the LF-derived timer onto a GPIO and count it with a calibrated frequency
  counter. Fast, and probing the GPIO does not load the crystal (probing the
  crystal directly would).
- Or measure accumulated drift over a long interval against a known reference —
  1 s of error over 24 h is 11.6 ppm, which is adequate resolution and needs no
  extra equipment.

Record the residual and the measurement date here once trimmed, as was done for
`&hfxo`.

**Anchor the day boundary away from working hours.** Define the TAN "day" as a

**Anchor the day boundary away from working hours.** Define the TAN "day" as a
24 h window starting at **04:00 local** rather than midnight. Clock error at the
boundary then falls in the small hours, where it can neither reject a working
engineer nor usefully extend a lost sheet into a working day. This costs one
constant and removes the entire practical impact of drift.

### 8.6 Power loss is the one real hole

System ON idle keeps the RTC running, but a **battery change or brown-out resets
it** and the day index is lost outright — a step error, not a slow one.

**The exposure:** if the device is unpowered from day N to day N+5 and resumes
from a day index persisted in NVS, it resumes believing it is day N, and day N's
sheet works again. A lost sheet's life can therefore be extended by cutting power.

**Why this does not justify an external RTC.** Rolling the date back requires
**hands-on physical access** to the battery. The threat being defended against is a
*lost TAN sheet* — a holder with BLE proximity but no physical access, against whom
day-scoping works perfectly. Anyone with hands-on access to a covert alarm sensor
can simply remove or destroy it, which defeats the product far more directly than
replaying a day's TANs. Spending a part, a backup cell and a supercap to close a
hole that only opens after the device is already physically compromised is poor
value.

**Revisit this if** the threat model changes to include an adversary with
repeated hands-on access who must nonetheless be kept out of the device's
data or configuration. In that case an RTC with a V<sub>BACKUP</sub> input
(RV-3028-C7, ~45 nA, negligible against a 69 µA budget) plus a supercap preserves
the date across a cell swap and also removes the drift concern.

**Firmware mitigations (required):**

- Persist the day index to NVS on each day rollover, and **never move it
  backwards** — on boot, take `max(persisted, synced)`. A monotonic day index means
  power loss can only ever *stall* the date, never rewind it below the last day the
  device actually saw.
- Resync on every hub/phone contact — inherit `alc_drawer_master`'s Settings
  characteristic `{"t":...}`.
- Log cold-power-up events so an unexplained battery interruption is visible to
  the operator as a tamper signal.

### 8.7 Recovery after a battery change — trusted provisioner time sync

Per §8.4 the device's clock gates entry, so a device that resumes on a stale day
would demand an out-of-date sheet, and a device with no date at all would be
**bricked**. `alc_drawer_master`'s hand-off notes carry an explicit anti-bricking
rule; the same discipline applies here.

**Resolution: a trusted provisioner supplies the time, authenticated by key.** A
provisioning credential distinct from the TAN seed is flashed at manufacture; a
holder of it can set the clock, and nothing else.

This unifies two cases that look different but are the same operation:
**first-ever boot at the factory** and **post-battery-change in the field** are both
"device has no trustworthy time, needs provisioning".

#### 8.7.1 The device does not need to advertise

The obvious construction is to have the device advertise on cold start so a
provisioner can find and connect to it. **That is not necessary, and it is worse.**
The device already scans every 6 s — that is an inbound channel. The provisioner
advertises a **signed time payload**, the device catches it in an ordinary scan
window and applies it. No mode switch, no advertising, no state machine.

What advertising on cold start would cost:

- **Covertness.** A covert sensor that advertises is discoverable. Cold start is
  reachable by anyone who can interrupt power.
- **A silent out-of-service failure mode — the serious one.** A brown-out with no
  attacker involved (an end-of-life cell drooping under a cold snap) would put the
  device into advertising and leave it there. It would broadcast indefinitely,
  waiting for a provisioner who is not coming, **performing no alarm function at
  all**. For a covert alarm sensor that is a safety failure, not an inconvenience.
- Power: continuous advertising at a 100 ms interval is roughly 50 µA, comparable
  to the entire operating budget (§3), so the device would also flatten the cell
  while doing nothing.

Payload size is not a constraint. Timestamp (4 bytes) + anti-replay counter
(4 bytes) + truncated HMAC (16 bytes) = 24 bytes, inside even a legacy 31-byte
advertising payload, and far inside extended advertising.

**If cold-start advertising is adopted anyway** (see §8.7.4), it must be **bounded**:
advertise for a fixed window, then fall back to scan-only using the persisted day
index, with short periodic re-advertise windows (10 s per hour ≈ 0.14 µA) to keep
the recovery path open. **Never advertise unbounded.**

#### 8.7.2 Address filtering is not authentication

Provisioning the **"ID" of a trusted provisioner is not sufficient** if that ID is a
BLE address. Addresses are unauthenticated at the link layer and trivially
spoofable, and a provisioner using a resolvable private address for privacy
rotates its address anyway, so matching on it requires an IRK and a bond rather
than a flashed constant.

**Provision a key, not an identity.** A 256-bit provisioning secret (or a
provisioner public key) in KMU, and require the time payload to carry a valid
HMAC/signature over `timestamp ‖ counter`. A filter accept list may sit on top as
an optimisation — that is how `alc_drawer_master` uses FAL, as a cheap pre-filter
in front of real HMAC authentication, never instead of it.

#### 8.7.3 Time must be monotonic, and forward jumps bounded

A stolen or replayed time sync is an attack on the TAN scheme itself: set the clock
to day N and a lost day-N sheet works again.

- **Never accept a time earlier than the persisted floor.** This kills replay of a
  captured sync outright, and is the same monotonic-day rule as §8.6.
- **Bound forward jumps.** Reject a sync more than a configured number of days ahead
  of the persisted day, except on a genuine cold start with no floor available.
  Without this, a holder of the provisioning key can push the clock years forward
  and deny service — the engineer's current sheet stops working. Note this is a
  denial of service only, not entry: generating a future-dated sheet needs the TAN
  seed, which the provisioning key does not confer.
- On cold start take `max(persisted, synced)`; the persisted index is the floor. A
  first-ever boot has no floor, which is correct — that is the factory case.

#### 8.7.4 Tool platform — Android, with the payload kept iOS-compatible

**Decision: the production engineer/provisioner tool is Android.** Arbitrary
manufacturer-specific advertising data is available, so §8.7.1 is adopted as
written: the tool advertises, the device stays scan-only for its entire life and
never advertises at all.

**Constraint retained anyway: both advertising payloads must fit 12 bytes.** This
keeps an iOS tool possible later without a protocol change, and it costs nothing —
see the budget below. Treat 12 bytes as a hard design limit, not a target.

Why 12: iOS `CBPeripheralManager.startAdvertising` accepts only
`CBAdvertisementDataLocalNameKey` and `CBAdvertisementDataServiceUUIDsKey` —
**manufacturer-specific data is unavailable**. A payload must be smuggled into a
128-bit service UUID. A legacy 31-byte advert holds flags (3) + one 128-bit UUID
(18) = 21 bytes with room left for a short name, giving 16 payload bytes; reserving
4 as a fixed magic prefix so the device can still filter adverts cheaply leaves
**12 usable**.

| Payload | Contents | Bytes |
|---|---|---|
| TAN | version (1) + 6-digit TAN (3) | 4 |
| Time sync | timestamp `uint32` (4) + truncated HMAC (8) | 12 |

Note the time sync needs **no anti-replay counter**: the monotonic floor rule in
§8.7.3 already rejects a replayed old timestamp, so those bytes go to the MAC
instead. A 64-bit MAC is ample — forging it is 2<sup>64</sup> work, and the device
accepts at most one attempt per 6 s scan window, so online brute force is not a
threat.

Two iOS caveats to record in case the option is ever taken up:

- **The app must be in the foreground.** Backgrounded iOS advertising drops the
  local name and moves service UUIDs into an overflow area not discoverable by
  non-Apple scanners. Acceptable for an attended engineer tool, fatal for anything
  unattended.
- Abusing a UUID field as a data field is non-standard and prevents filtering on
  the full UUID — hence the fixed 4-byte prefix above.

#### 8.7.5 Bench development uses a dongle, not a phone

For bring-up, the advertiser should be an **nRF52840 dongle or a spare
nRF54L15-DK** driven from the host, not a phone of either platform. It is
scriptable and reproducible, removes an app build from the edit-test loop, and —
importantly — allows deliberately malformed, replayed and out-of-range payloads to
be injected to test the monotonic floor and bounded-forward-jump rules of §8.7.3.
Those paths are difficult to exercise from a well-behaved phone app.

## 9. Open hardware item

A fresh CR123A can sit at 3.2–3.3 V open-circuit against the nPM2100's **3.4 V
V<sub>BAT</sub> maximum**. Within specification, but confirm against the datasheet
of the specific cell selected before committing to the design.

## 10. Method note

Averages are computed as charge per cycle divided by cycle period; continuous
contributors are added directly. With the nPM2100 in pass-through for the bulk of
the CR123A discharge (BOOST enters pass-through when V<sub>BAT</sub> exceeds
target V<sub>OUT</sub> by 100 mV), battery current ≈ load current with no boost
conversion loss, so the 3 V component figures above can be used directly against
cell capacity. Once V<sub>BAT</sub> falls below the V<sub>OUT</sub> target the
boost engages and draws proportionally more from the cell; at the low end of the
CR123A curve this affects a small fraction of total capacity and is covered by
the 1500 → 1450 mAh derating.
