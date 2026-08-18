# MFS_1 Battery-Life Test

Branch `feat/battery-test`. Recorded 2026-08-18.

Measures real CR123A life at the 6 s / 100 ms passive scan duty cycle, against the
~69 µA predicted in `docs/power-budget.md` §3.

## 1. Behaviour

Identical to `main` except for LED A:

| | `main` | This branch |
|---|---|---|
| LED A | On while Inactive | **Blink `CONFIG_MFS_BLINK_MS` once per scan period** |
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

## 3. Before starting the test

- **Detach the debugger.** This matters more than anything else here. An attached
  SWD probe holds debug power domains alive and can add hundreds of µA to
  milliamps, swamping the entire measurement. Flash, then physically disconnect.
- **Consider `CONFIG_LOG=n`.** RTT is only RAM writes and costs little, but string
  formatting on each event is avoidable work. Left on for now so a fault is
  diagnosable afterwards.
- **Fit a known cell** and record its make, nominal capacity and date. 1450 mAh is
  assumed in the budget.
- **Leave it Inactive** unless testing armed behaviour. Motion events wake the
  ADXL367 out of its ~180 nA wake-up mode into measurement mode, which changes the
  average.
- **Record the start time and the ambient temperature.** CR123A capacity is
  temperature-dependent, and a bench near a window is not a covert install.

## 4. What the result tells you

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
