# Fire supply on demand — amendment (owner, 2026-11-14)

Amends `2026-09-14-arming-sequence-amendment.md` and the fire gate drive rule in
`CLAUDE.md` (bench, 2026-11-12).

## 1. Why

Arming raised the nPM2100 boost to 3.3 V and held it there for the whole armed
session. Measured on the CLASS MFS board (PPK2 in ammeter mode, battery supply,
armed, 10 s window spanning a scan): **161 µA average**, the trace showing the
boost switching throughout. That is about 40 µA over the ~117 µA budget
(`docs/power-budget.md`), roughly 117 µA × 1.1 (3.0 → 3.3 V) ÷ ~0.8 boost
efficiency at this load. Armed is the deployed state, so the cost runs for the
battery's whole life: ~1.4 years falls to ~1.0 year.

## 2. Rule

Armed, the boost idles at its VSET level. It is raised to 3.3 V **only when a
fire is pending**:

| Fire path | When the supply is raised |
|---|---|
| Live FIRE command (Active) | When the FIRE is accepted, at the top of its 10 s countdown |
| Armed trigger with a delay | When the delay starts (`DetectionEventType::DelayStarted`, armed) |
| Armed trigger without a delay | Just before the output asserts: raise → wait 100 ms → assert |

The last row is also the backstop for every path: `App::updateOutputState()`
calls `prepareFireSupply()` whenever the derived output is asserted, before
`OutputSwitch::Set(true)`. One attempt per pending fire (`m_fire_supply_raised`,
cleared by `LowerFireSupply()`), so the 100 ms settle never repeats.

A rehearsal FIRE never raises it.

## 3. Owner decisions

1. **Keep a battery test at arming** (owner, 2026-11-14). The arming step still
   raises the supply and verifies VOUT ≥ 3.2 V, then lowers it at once before
   the pins are enabled: `RestartDetection(true) → RaiseFireSupply() →
   LowerFireSupply() → EnableFirePins() → Active`. A cell that cannot hold the
   firing level still refuses arming on site, not at the moment of need.
2. **Fire anyway** when the supply is not verified at fire time — a failed boost
   write, a failed VOUT read or VOUT below 3.2 V (owner, 2026-11-14). Logged
   (`Fire supply not verified at the firing level (%d) - firing anyway!`), never
   refused. A weak gate drive may still fire the e-match; aborting is no safer,
   and a raised supply with isolated pins is not a hazard. This is deliberately
   not a fail-safe: the always-fail-safe rule covers faults that threaten
   control of the output, which this does not.

## 4. Unchanged

- Every disarm and fail-safe lowers the supply straight after the pins are
  disabled (`ArmingSequence`), including the boot disarm. A trigger completes
  through the ordinary disarm, so the supply falls after every fire.
- The fire stagger, the interlock and the single derivation point.
- The settle time is now 100 ms (`M_FIRE_SUPPLY_SETTLE_MS`, owner), for both the
  battery test and the fire-time raise; the boost settles in well under that.

## 5. Verification

- Host: `testFireSupplyTestedBeforeEnable` (raise then lower, both before the
  enable, still Arming). The fire-time raise lives in `App` and is bench-checked.
- Bench: checklist §5i.
