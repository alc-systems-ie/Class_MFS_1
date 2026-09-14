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

## 2. Scan period 5876 ms

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
window W and N consecutive scans (30 000 ms / P ≈ 5.1 at these periods, so **N = 5**),
and a counterpart advertising interval I modelled as an instant repeating every I ms at
an unknown start phase φ: scan k opens a window [kP, kP + W], which catches an advert at
phase φ + jI exactly when (φ − kP) mod I ≤ W. The script sweeps 2000 phase samples over
one interval [0, I) and reports the fraction for which none of the N windows catch it —
the fraction of commands, starting at a uniformly random moment, that would be missed
entirely. An interval I ≤ W is exempt (a single window always contains an advert). The
script's docstring has the full derivation and its limitations (instantaneous adverts,
no advDelay jitter — both of which make the model pessimistic, not optimistic).

Results at W = 100 ms, N = 5, 2000 phase steps, for the default interval set (measured
and plausible counterpart intervals):

| Period | 152.5 | 187.5 (Mac, measured) | 211.25 (Apple rec.) | 318.75 | 417.5 | 546.25 | 760 | 852.5 | 1022.5 | 1285 |
|---|---|---|---|---|---|---|---|---|---|---|
| 6000 ms | PASS | **FAIL 47%** | PASS | PASS | FAIL 5% | FAIL 75% | FAIL 45% | FAIL 73% | FAIL 51% | FAIL 75% |
| 5905.625 ms | PASS | PASS | **FAIL 35%** | FAIL 21% | FAIL 18% | FAIL 8% | FAIL 39% | FAIL 59% | FAIL 51% | FAIL 61% |
| **5875.625 ms (new default)** | PASS | **PASS** | **PASS** | **PASS** | FAIL 47% | FAIL 24% | FAIL 40% | FAIL 45% | FAIL 59% | FAIL 61% |

(35 ms and 100 ms are exempt at every period, I ≤ W.) 5875.625 ms (9401 BLE units) is
the shortest period at this window/N that clears every measured or plausible interval up
to 318.75 ms; the sweep in the script's `--sweep` mode shows the pass region ends at
9403 units and the next unit above it already fails against 318.75 ms. Window stays
100 ms; average current rises by 6000/5876 on the scan terms (~2.1 %), negligible
against the ~69 µA budget (`docs/power-budget.md` §3).

**Intervals from about 5 × W upward cannot be fully covered by any period at N = 5.**
546.25 ms and above fail at all three periods above: a single 100 ms window can
intersect at most one cycle of an advertiser that slow per scan, so five scans sweep at
most 5 × W = 500 ms of the I − W gap the window does not already cover — for I well past
1000 ms that is a small fraction of the gap, and no choice of P closes it. This is not a
period-tuning problem; it is why the app's foreground-only, fast-advertising guarantee
(§6, owner decision) is the actual protection against a slow or backgrounded advertiser,
not this scan period. The production Android phone's interval must still be measured
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

Following review of §2 and the "OPEN" item it raised, the owner made three decisions
that together close the gap a period alone cannot:

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
  period: the app never leaves the counterpart advertising fast in the background for
  the scanner to contend with.
- **Scan period 5876 ms, checked by simulation.** §2 above; `CONFIG_MFS_SCAN_PERIOD_MS`
  default 5876 (9401 BLE units, 5875.625 ms), verified with `tools/scan_phase_check.py`
  rather than the withdrawn closed-form rule.

Together these replace the §2 "OPEN — 211.25 ms fails" item: 5876 ms now passes against
211.25 ms directly (see the table in §2), and the no-scanner/no-arming and
foreground-only rules remove the two ways a slower or backgrounded advertiser could
otherwise leave the device unable to hear a Disarm.
