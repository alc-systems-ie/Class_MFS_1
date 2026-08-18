# MFS_1 Battery-Life Test

Branch `feat/battery-test`. Recorded 2026-08-18.

Measures real CR123A life at the 6 s / 100 ms passive scan duty cycle, against the
~69 µA predicted in `docs/power-budget.md` §3.

## 1. Behaviour

Identical to `main` except for LED A:

| | `main` | This branch |
|---|---|---|
| LED A | On while Inactive | **Never lit** (`CONFIG_MFS_BLINK_MS=0`), or blink if set |
| LED B | On while Active **and** triggered, ~5 s | Unchanged |
| Start state | Inactive | Unchanged |
| Arming | Engineer toggle from the DK | Unchanged |

Enabled by `CONFIG_MFS_BATTERY_TEST=y`, so `main`'s behaviour is one Kconfig away.

**The blink is not phase-locked to the radio.** The controller duty-cycles the scan
itself from the interval/window pair and gives the application no callback at
window start, so this is an independent software tick of the same 6 s period. It
proves the device is alive and cycling; it does not mark the instant the receiver
opens. Locking the two together would mean driving the scan start/stop from
firmware, which changes the very thing being measured.

## 2. THE BLINK DOMINATES THE MEASUREMENT

At 500 ms per 6 s the LED is on **8.3% of the time**. Against a ~69 µA operating
budget:

| LED current | Blink contribution | Total | CR123A life | What you are measuring |
|---|---|---|---|---|
| 1 mA | 83 µA | 152 µA | ~1.1 years | mostly the LED |
| **2 mA** | **167 µA** | **236 µA** | **~8 months** | mostly the LED |
| 5 mA | 417 µA | 486 µA | ~4 months | almost entirely the LED |

Against a no-LED prediction of **~69 µA → 2.4 years**. So a 500 ms blink makes the
result roughly **3× pessimistic at 2 mA**, and the number obtained is a property of
the indicator rather than of the scan.

**Measure the LED current first.** Everything above is an assumption until the
actual per-LED current on this board is known; it depends entirely on the series
resistor.

### Options

| Blink | Cost at 2 mA | Visible? | Use when |
|---|---|---|---|
| 500 ms | ~167 µA | Obvious across a room | Only if the LED cost is being subtracted |
| 100 ms | ~33 µA | Clearly visible | **Suggested compromise** |
| 10 ms | ~3 µA | Visible up close in dim light | Needs `M_POLL_INTERVAL_MS` shortened to resolve |
| Off | 0 | Not | The honest measurement — pair with a second board that blinks |

**Recommended:** run **two boards** — one at `CONFIG_MFS_BATTERY_TEST=n` for the
real figure, one blinking as a live indicator that the firmware is still running.
Failing that, use 100 ms and subtract the measured LED contribution.

Note the blink is quantised to `M_POLL_INTERVAL_MS` (100 ms), so
`CONFIG_MFS_BLINK_MS` rounds down to a multiple of 100 ms.

## 2.1 Configured for the true figure

`CONFIG_MFS_BLINK_MS=0`. LED A is **never lit** — not blinking, and not lit as an
Inactive indicator either. LED B still works, so the liveness check is: **arm the
device from the DK, then nudge it.** LED B lights for ~5 s.

Arming does not change the idle current — the arm state is a firmware flag, and
the ADXL367 sits in autosleep wake-up mode either way. So the device can be left
Active for the whole run. Note it returns to Inactive on any power cycle, so it
needs re-arming after one.

## 3. Measuring with a Power Profiler Kit II

### Which mode

Nordic's nPM2100 EK guidance is explicit about this:

> Set PPK2 in **Ampere Meter mode** to measure the power output VOUT and in
> **Source Meter mode** to measure power input **IBAT**.

Battery-input current is what matters here, so **use Source Meter mode**: the PPK2
*replaces* the cell and supplies the board, measuring what the nPM2100 draws.

| Mode | Wiring | Use for |
|---|---|---|
| **Source Meter** | PPK2 replaces the cell and powers the board | **Battery draw — use this** |
| Ampere Meter | PPK2 in series between an external supply and the board | VOUT-side rails, or if the real cell must stay in circuit |

Source Meter is the better choice for a second reason: the supply voltage becomes
a variable you control. A CR123A starts near 3.0 V and falls toward 2.0 V, and
consumption is not flat across that range — the nPM2100 leaves pass-through and
starts boosting as V<sub>BAT</sub> drops, drawing proportionally more. Measuring at
3.0 V alone will flatter the result.

### Source Meter connections

1. **Remove the cell.** The PPK2 is the supply; leaving the cell in parallel
   invalidates the measurement and can back-feed the PPK2.
2. **PPK2 `VOUT` → the board's battery positive** (nPM2100 V<sub>BAT</sub>, i.e.
   the cell holder's + terminal).
3. **PPK2 `GND` → the board's ground** (the cell holder's − terminal).
   `VIN` is unused in this mode.
4. **Disconnect the J-Link.** An attached debugger holds debug power domains alive
   and can add hundreds of µA to milliamps. This single point can invalidate the
   whole measurement.
5. PPK2 to the computer over USB, open the **Power Profiler** app, select the
   PPK2, choose **Source Meter**, set the voltage, toggle **Enable power output**,
   then **Start**.

### What to set and what to expect

| Supply | Emulates | Expected average |
|---|---|---|
| 3.0 V | Fresh CR123A | ~69 µA predicted |
| 2.5 V | Mid-life | Higher — boost engaging |
| 2.0 V | Near end of life | Higher again |

Sweeping those three gives a far better life estimate than a single point, and
takes minutes rather than months.

The trace should show a ~4 µA floor with a **100 ms burst every 6 s** at a few mA
— that burst is the scan window, and confirming its width and period directly
validates the duty cycle the whole budget rests on.

### Cautions

- The nPM2100 needs the supply to provide **at least 10 mA during cold start**
  (datasheet V<sub>BATCOLD_START</sub>). The PPK2 handles this easily, but do not
  current-limit it below that.
- PPK2 measures from ~200 nA to 1 A. The ~69 µA average is comfortable; the ~4 µA
  sleep floor is fine, though sub-µA accuracy degrades — do not read too much into
  the last decimal of the floor.
- Let it settle for several minutes before trusting an average — the ADXL367 needs
  to reach autosleep, and any handling during setup keeps it in measurement mode.

## 4. Before starting the test

- **Detach the debugger.** This matters more than anything else here. An attached
  SWD probe holds debug power domains alive and can add hundreds of µA to
  milliamps, swamping the entire measurement. Flash, then physically disconnect.
- **Consider `CONFIG_LOG=n`.** RTT is only RAM writes and costs little, but string
  formatting on each event is avoidable work. Left on for now so a fault is
  diagnosable afterwards.
- **Fit a known cell** and record its make, nominal capacity and date. 1450 mAh is
  assumed in the budget.
- **Leave it Active and undisturbed.** Arming costs nothing (it is a firmware
  flag), and it means a nudge lights LED B as the liveness check. Motion itself
  does cost: it wakes the ADXL367 out of its ~180 nA wake-up mode into measurement
  mode, so check it rarely and note when you did.
- **Record the start time and the ambient temperature.** CR123A capacity is
  temperature-dependent, and a bench near a window is not a covert install.

## 5. What the result tells you

The predicted 2.4 years assumes the component figures in `docs/power-budget.md` §2
and, critically, that the nPM2100 is in pass-through for the bulk of the discharge.
A measured result significantly worse than predicted, with the LED contribution
subtracted, most likely points at:

- Boost mode — verify it is in pass-through and not switching, since that is worth
  most of the efficiency assumption.
- LDOSW left in High Power rather than dropping to Ultra-Low Power after ADXL
  configuration (`App::lowerLsoutToUlp()`).
- The scan window being longer in practice than the configured 100 ms.
- The ADXL367 sitting in measurement mode rather than autosleep's wake-up mode —
  check it is not being re-triggered by ambient vibration.

Because full discharge takes months, consider also measuring instantaneous current
directly with a Power Profiler Kit on a bench supply. That gives the answer in
minutes and validates the model; the long test then only has to confirm it.
