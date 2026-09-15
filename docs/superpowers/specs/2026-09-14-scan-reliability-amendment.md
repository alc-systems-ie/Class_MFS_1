# MFS_1 Scan Reliability, Send/Stop — Design Amendment

**Date:** 2026-09-14 (bench session 1)
**Amends:** `docs/power-budget.md` (scan period), `docs/superpowers/specs/2026-09-14-arming-sequence-amendment.md` §3 (scanning during Arming), `docs/superpowers/specs/2026-09-12-app-control-design.md` §3 and §8 (Send UI), `docs/superpowers/specs/2026-09-14-command-types-amendment.md` §3 (pre-empt removed). Where this document and those disagree, **this document wins**.

## 1. Measurements (bench, 2026-09-14)

A second nRF54L15 DK ran a continuous passive observer (`tools/uuid_observer`) logging
every 128-bit service UUID advert with a timestamp; MFS_1 ran `CONFIG_MFS_SCAN_DIAG`
reception counters.

| Advertiser (class app) | Interval | 30 s advert | MFS_1 catch per 100 ms scan |
|---|---|---|---|
| macOS (MacBook, foreground) | **187.5 ms**, steady | ~155 adverts | 0 or 1; **whole commands missed** |
| iPhone (iOS 26.6.1, foreground, release build) | **~35 ms**, steady for 30 s | ~800 adverts | 2–3, **every scan** |

**Root cause of today's unreliable commands (Mac):** 6000 ms = exactly 32 × 187.5 ms.
Every scan lands at the same phase of the Mac's advertising cycle, drifting only a few
ms per scan, so a command is caught on almost every scan or on almost none. Observed:
a time sync missed by all 6 of its scans; four consecutive Disarms unheard.

**Replay of the previous advert (iOS):** when a new advert starts, iOS briefly (1.7 s
observed) re-broadcasts the previous advertising payload before the new one, even a
minute after the previous advert ended.

## 2. Scan period 5970 ms (initially set to 5876 ms)

`CONFIG_MFS_SCAN_PERIOD_MS` default **6000 → 5876**. 6000 ms is exactly 32 × 187.5 ms,
the advertising interval measured from a macOS advertiser, so every scan landed at the
same phase of its cycle and whole commands were missed.

An earlier value, 5906 ms (9449 BLE units, 5905.625 ms), was chosen against a
closed-form rule — for every counterpart interval I longer than the scan window W, the
per-scan phase drift d = min(P mod I, I − (P mod I)) must satisfy d × N ≥ I − W — and
reported a pass against the measured 187.5 ms interval. Review found the rule unsound:
it is off by one (it does not correctly bound the worst-case start phase) and its own
derivation assumes d ≤ W, which does not hold in general. A brute-force simulation
confirmed the rule's own result was wrong in practice: 5905.625 ms misses **35 %** of
commands against Apple's recommended 211.25 ms interval, not the pass the rule had
reported for the case it was checked against, and the rule had already flagged 211.25 ms
as a separate open failure before that. **The closed-form rule is withdrawn everywhere**
in this project's documentation; the period is chosen and checked by simulation only.

**Method: phase-coverage simulation, `tools/scan_phase_check.py`.** For a scan period P,
window W and N consecutive scans, and a counterpart advertising interval I modelled as an instant repeating every I ms at
an unknown start phase φ: scan k opens a window [kP, kP + W], which catches an advert at
phase φ + jI exactly when (φ − kP) mod I ≤ W. The script sweeps 2000 phase samples over
one interval [0, I) and reports the fraction for which none of the N windows catch it —
the fraction of commands, starting at a uniformly random moment, that would be missed
entirely. An interval I ≤ W is exempt (a single window always contains an advert). The
script's docstring has the full derivation and its limitations.

**N is derived, not assumed.** A 30 000 ms command burst starts at an arbitrary moment
relative to the scan schedule, so the first window inside it opens anywhere from 0 to P
after the burst starts. The number of complete windows *guaranteed* inside any burst of
length B is **N = floor((B − W) / P)** — window k fits when (k + 1)P + W ≤ B in the worst
case. The script derives it per period (`--burst-ms`, default 30 000; `--scans`
overrides). **N = 5 needs 5P + W ≤ 30 000 ms, i.e. P ≤ 5980 ms. Raising the period past
5980 ms drops N to 4**, which none of the passing islands below survive. 5970 ms has
10 ms of that margin, and an advert that starts late eats into it.

**A PASS holds for a steady interval; real advDelay jitter can add a small miss rate.**
Earlier wording called the steady model pessimistic. It is not: a passing period works by
stepping the scan phase across the advertiser's cycle in exact increments, and the BLE
advDelay (0–10 ms, pseudo-random, added to every advertising event) perturbs those steps.
`--jitter-ms J` adds a Monte Carlo model — each advert follows the previous one by
I + U(0, J), the burst starts at a random point before the first scan, `--trials` trials.
Read literally, I + U(0, J) has a mean spacing of I + J/2, which treats I as a *nominal*
interval; `--jitter-centred` draws I − J/2 + U(0, J) instead, a mean of exactly I, which
treats I as a *measured* mean spacing. The Mac's 187.5 ms is a measured spacing — the
6000 ms scan stayed phase-locked to it across whole commands, which a mean 5 ms longer
could not do — so the centred model is the one supported for the measured interval.

**Jitter results must use the model that matches each interval's source, not one model
for all of them.** The Mac's 187.5 ms is a *measured* mean spacing (§1: the 6000 ms scan
stayed phase-locked to it across whole commands, which a mean 5 ms longer could not do),
so it takes the centred model (`--jitter-centred`, mean exactly I). Apple's nominal
152.5/211.25/318.75 ms are *nominal* advInterval values, for which the real spacing is
I + advDelay — the uncentred model (mean I + J/2), no `--jitter-centred`. An earlier
version of this table applied the centred model to every interval, including the nominal
ones; that understates their miss rate, since it treats advDelay as centred on the
interval rather than added on top of it. Corrected, matched-model results, J = 10 ms,
20 000 trials, seed 1 (`tools/scan_phase_check.py`; reproduce with `--trials 20000`):

| Period | 187.5 (Mac, measured, centred) | 152.5 (nominal) | 211.25 (nominal) | 318.75 (nominal) | Worst |
|---|---|---|---|---|---|
| **5970.000 ms (9552)** | 1.28 % | 4.94 % | 0.02 % | 6.46 % | **6.46 %** |
| 5862.500 ms (9380) | 0.00 % | 0.01 % | 11.38 % | 26.16 % | **26.16 %** |
| 5875.625 ms (9401) | 0.00 % | 0.00 % | 1.04 % | 10.18 % | **10.18 %** |

(Steady-model verdict for all three periods against all four intervals is PASS; only the
jitter miss rates differ.)

**Under the matched models, 5970 ms (9552 units) has the best worst case of the three** —
6.46 %, against 9380's 26.16 % and 9401's 10.18 %. The sweep ranking previously quoted
here (9380 units "first at 0.29 %") used the centred model uniformly, including for the
nominal intervals, and does not hold once each interval is matched to its correct model —
it is withdrawn. **No period is clean against every interval once jitter is counted**:
every row above has a non-zero worst case, and which interval dominates (and so which
period ranks best) depends on the model. A few ms of uncertainty in the counterpart's
*mean* interval matters more than the period choice — another reason the app's
foreground, fast advertising (§6), not the period, is the guarantee. **The scan period
must still be re-checked against the production Android phone's own MEASURED mean
interval (centred model) once it is captured on the bench** (bench checklist §5d) — a
nominal spec figure is not a substitute for that measurement. The period is unchanged
here; this is recorded for the owner.

Steady-model results at W = 100 ms, N = 5, 2000 phase steps, for the default interval set
(measured and plausible counterpart intervals). N = 5 is the derived value (below) for
every row except 6000 ms, where the derived N is 4 and the row is worse still (318.75 ms
then misses 16 %); reproduce that row with `--scans 5`:

| Period | 152.5 | 187.5 (Mac, measured) | 211.25 (Apple rec.) | 318.75 | 417.5 | 546.25 | 760 | 852.5 | 1022.5 | 1285 |
|---|---|---|---|---|---|---|---|---|---|---|
| 6000 ms | PASS | **FAIL 47%** | PASS | PASS | FAIL 5% | FAIL 75% | FAIL 45% | FAIL 73% | FAIL 51% | FAIL 75% |
| 5905.625 ms | PASS | PASS | **FAIL 35%** | FAIL 21% | FAIL 18% | FAIL 8% | FAIL 39% | FAIL 59% | FAIL 51% | FAIL 61% |
| 5875.625 ms (superseded) | PASS | **PASS** | **PASS** | **PASS** | FAIL 47% | FAIL 24% | FAIL 40% | FAIL 45% | FAIL 59% | FAIL 61% |
| **5970.000 ms (current default)** | **PASS** | **PASS** | **PASS** | **PASS** | FAIL 12% | FAIL 53% | FAIL 34% | FAIL 87% | FAIL 51% | FAIL 64% |

(35 ms and 100 ms are exempt at every period, I ≤ W.) 5875.625 ms (9401 BLE units)
**passed every measured or plausible interval up to 318.75 ms** — it was not the
*shortest* period with that property, and passing periods are not unique: sweeping
`--sweep` over a wide range against this interval set turns up several other passing
islands, for example around 9335 units (5834.375 ms) and 9537–9565 units
(5960.625–5978.125 ms). 9401 was simply the owner's first choice — chosen from inside a
passing island wide enough to have margin, not because it was extremal in any sense.

**Margin at 9401.** Sweeping the 61 units either side of it (9371–9431) shows the
island containing 9401 runs from **9355 to 9403 units (5846.875–5876.875 ms)** — a
49-unit-wide (30.6 ms) contiguous run that all pass against every interval up to
318.75 ms — before failing at 9404 units (0.8% against 318.75 ms, rising steadily
with distance from the island). 9401 sat 3 units from that island's upper edge and
46 from its lower edge, so it had some margin against a future re-measurement nudging
an interval slightly, but not a great deal on the upper side.

**Superseded 2026-09-14 (follow-up task): moved to 9552 units (5970.000 ms).**
"Less battery is always good" — re-sweeping (`--sweep 9530 9570 --max-interval-ms
320`) confirmed a second, wider passing island at **9537–9565 units
(5960.625–5978.125 ms)**, 29 units wide against the first island's 49 (but with the
narrow edge nowhere near either side): the owner picked its middle, **9552 units
(15 units of margin below, 13 above)**, both to get away from 5876 ms's 3-unit edge
margin and because 5970 ms sits closer to the unconstrained 6000 ms optimum, which
lowers the scan-power term slightly versus 5876 ms (see below). `--period-units
9552` reports 0 % miss against 152.5/187.5/211.25/318.75 ms; the immediate
neighbours outside the island fail — 9536 misses 152.5 ms at 1.6 %, 9566 misses
187.5 ms at 1.35 % — confirming the edges. `CONFIG_MFS_SCAN_PERIOD_MS` default and
`tools/scan_phase_check.py`'s `DEFAULT_PERIOD_UNITS` both move to **9552**. Re-run
the sweep before assuming a similar value stays safe if intervals are re-measured.

Window stays 100 ms; average current rises by 6000/5970 on the scan terms (~0.5 %),
negligible against the ~69 µA budget (`docs/power-budget.md` §3) — a smaller rise
than 5876 ms's 6000/5876 (~2.1 %), since 5970 ms is closer to 6000 ms.

**Intervals from about 5 × W upward cannot be fully covered by any period at N = 5.**
546.25 ms and above fail at all four periods above: a single 100 ms window can
intersect at most one cycle of an advertiser that slow per scan, so five scans sweep at
most 5 × W = 500 ms of the I − W gap the window does not already cover — for I well past
1000 ms that is a small fraction of the gap, and no choice of P closes it. This is not a
period-tuning problem; it is why the app's foreground-only sending (§6, owner decision) —
which keeps the counterpart advertising fast, on Android through `ble_peripheral` 2.4.0's
`ADVERTISE_MODE_LOW_LATENCY` — is the actual protection against a slow or backgrounded
advertiser, not this scan period. The production Android phone's interval must still be measured
with `tools/uuid_observer` and checked with the script (bench checklist §5d).

## 3. Continuous scanning during Arming

While the state is **Arming** the scanner runs **continuously** (the same cadence an
armed trigger delay uses), so a Disarm sent during the 10 s exit delay is heard within
a fraction of a second of the phone advertising. It returns to duty-cycled scanning
when Arming ends (Active, cancelled, or failed) unless an armed trigger delay still
needs it.

**One arbiter:** the requested cadence is `fast = (state == Arming) || armedDelayPending`,
decided in one pure function and applied in one place in `App`; the detection engine's
`SetTriggerPendingScan` request becomes one input to it. The PM lock stays tied to the
armed trigger delay only. Scanner failures keep today's handling and retry.

## 4. App: Send and Stop

Every page that sends (Arm page, Settings page, provisioner) has **Send** and a red
**Stop**:

- **Idle:** Send enabled (subject to Bluetooth and page rules), Stop dimmed.
- **Send pressed:** Send dims immediately; Stop becomes active once advertising has
  started (or at once, cancelling the start).
- **Stop pressed, or the 30 s window ends:** advertising stops; Stop dims; Send
  becomes active.

The "Send Disarm (replaces advert)" pre-empt is **removed**. To disarm straight after
arming — or to cancel an arming — the engineer presses **Stop**, sets Disarmed, and
presses **Send**. The disarm prompt still appears only for a Disarm Send that started
advertising. The legend explains Stop.

## 5. Replay of a previous command

Because the platform can re-broadcast the previous payload for a moment when a new
advert starts (§1), a command that was **never heard** can reach the device just
before the next one. Consequences, all acceptable:

- The replayed command is authentic, fresh-checked (10-minute window) and consumes its
  own sequence number; the new command, with a higher `n`, follows.
- A spent command replays silently (not for us).
- The worst realistic case is an unheard Settings or Arm acted on moments before the
  engineer's next command; the next command still takes effect after it. Stop (§4)
  lets the engineer end an advert once LED A confirms, which reduces what can linger.

No firmware change; documented so an unexpected acknowledgement is explicable.

## 6. Owner decisions 2026-09-14

Following review of §2 and the "OPEN" item it raised, the owner made three decisions.
Only the second of these — foreground-only, fast-advertising sending — closes the gap a
period alone cannot; the other two protect different failure modes (see the closing
paragraph below):

- **No scanner, no arming.** An Arm is refused when the scanner is not running, both
  when the Arm is accepted (no Arming state entered) and, fail-safe, at the end of the
  exit delay before the armed restart — a device that could not hear a Disarm must never
  reach Active. The refusal raises the warning pattern (LED B), names the scanner as the
  reason, and gives no LED A acknowledgement; the device stays Inactive with pins
  isolated and the detection test running. Implemented in the host-tested
  `ArmingSequence` via `ArmingActions::ScannerRunning()` — never a second check in `App`.
- **Foreground-only sending, enforced by the app.** Send is disabled unless the app
  lifecycle is `resumed`; when the app becomes `hidden`, `paused` or `detached`, any
  advert (or a pending start) is stopped at once and the page shows "Advertising
  stopped - keep the app open while sending." `inactive` (visible but unfocused — a
  macOS window losing focus, or an iOS system sheet) does **not** stop an advert; that
  state is common and momentary, not a reason to interrupt a Send. This is what
  actually bounds the slow-interval case §2 identifies as uncoverable by any scan
  period: a Send only ever advertises from the foreground, where the platform
  advertises fast (on Android, `ble_peripheral` 2.4.0 requests
  `ADVERTISE_MODE_LOW_LATENCY`), never at a background interval.
- **Scan period 5970 ms, checked by simulation.** §2 above; `CONFIG_MFS_SCAN_PERIOD_MS`
  default 5970 (9552 BLE units, 5970.000 ms — initially set to 5876 ms, 9401 units,
  5875.625 ms, then moved by the owner to the middle of a wider passing island,
  9537–9565 units), verified with `tools/scan_phase_check.py` rather than the
  withdrawn closed-form rule.

Together these replace the §2 "OPEN — 211.25 ms fails" item: 5970 ms now passes against
211.25 ms directly for a steady advertiser (see the table in §2; jitter adds a small miss
rate). Only the foreground-only rule bounds the slow- or backgrounded-advertiser case. No
scanner, no arming does not: it guarantees the device can *hear* at all while armed, not
that a slow advertiser is caught.
