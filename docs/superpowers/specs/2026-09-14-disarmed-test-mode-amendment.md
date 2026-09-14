# MFS_1 Disarmed Test Mode — Design Amendment

**Date:** 2026-09-14 (bench session 1)
**Amends:** `docs/superpowers/specs/2026-09-14-command-types-amendment.md` §2.2 (Inactive rows);
`docs/superpowers/specs/2026-09-12-app-control-design.md` §6.3, §6.5.1; `docs/v1-scope.md`
§1.0.1 (the standby-while-deactivated mechanism). Where this document and those disagree,
**this document wins**.

## 1. The finding

Bench, 2026-09-14: after a Disarm, taps did nothing. While Inactive the ADXL367 was
held in standby, and the detection engine ran only once a Settings command started a
tuning session. The Settings page opens at defaults (the app never knows the device's
settings), so to watch LED B the engineer had to re-enter every value, or a Send would
overwrite the tuning. Without RTT the engineer is lost.

## 2. The rule (owner, 2026-09-14)

**The disarmed state differs from the armed state in exactly two ways:**

1. **Changing parameters is only possible while disarmed.** (Unchanged — command types
   amendment §2.2.)
2. **A trigger is indicated on LED B while disarmed; while armed it asserts the fire
   GPIOs.**

Everything else — activation counting, cooldown, the delay before triggering, the
detection period — runs identically in both states.

Owner decisions on the details:

- **The delay runs while disarmed**, on the same GRTC `k_timer`, so LED B lights only
  after the configured delay, as the GPIOs would. **Continuous scanning and the PM lock
  are armed-only**: nothing dangerous is pending on a disarmed device, so a 9 h test
  delay costs no extra battery.
- **LED B lights on detection in both states** on bench builds (`CONFIG_MFS_DEBUG_LED`),
  as today. Production builds have no LED B.

## 3. Behaviour

| Event | Result |
|---|---|
| Boot | Inactive, **testing** at the stored settings |
| Disarm (from Active or Inactive) | Inactive, **test restarted from zero** |
| Settings (Inactive) | Settings applied, **test restarted from zero** |
| Trigger completes while armed | One-shot: latched Inactive, **test restarted from zero** |
| Trigger completes while disarmed | Test carries on; the count is already zero |
| Arm (Inactive) | **Fresh session**, exactly as today, then Active |
| Arm or Settings while armed | Replay Armed; nothing changes (unchanged) |

**"Restarted from zero"** means everything the previous session held is discarded:
activation count, detection latch and hold, cooldown (PMIC timer stopped, expiry latch
cleared, retry state cleared), pending delay (timer stopped), one-shot flags, stuck-AWAKE
watchdog ticks, and the accelerometer reconfigured through the loop-mode bootstrap with
the stale-AWAKE suppression. **Arming performs the same restart**, then sets the boolean
only if the accelerometer configured. Otherwise Arm Refused: the device stays Inactive,
and the engine retries the configure at 1 Hz (the existing cooldown re-arm retry path) so
the test resumes as soon as the part responds.

**LED A on Disarm:** the double blink ("a pending trigger was cancelled") plays only
when an **armed** delay was pending. Cancelling a test delay is not a cancelled trigger,
so it plays the ordinary slow flash. Log lines for a disarmed delay say it is a test
(e.g. `TEST trigger pending: LED B in N s.`), never `TRIGGER PENDING`.

**Acknowledgement latency:** because the restart's loop-mode bootstrap waits for AWAKE to
clear before returning (up to about the configured inactivity period plus 3 s — ~8 s at
the 5 s default), a Disarm or Settings acknowledgement on LED A can be delayed that long
while the device is being handled, even though the output is already off.

### 3.1 Resetting a long test

**Send Disarm.** It restarts the test from zero at any point — mid-count, mid-cooldown
(up to 1 h) or mid-delay (up to 9 h). Sending Settings also restarts it. The app says
so on the Settings page.

### 3.2 Arming mid-test — SAFETY CRITICAL

An engineer may arm at any moment of a test: with activations part-counted, during a
cooldown, during a pending test delay, during a test detection period (LED B lit), or
while handling the device (AWAKE asserted). **In every case the armed session starts
from zero and no state from the test can reach the fire GPIOs.** Specifically, after
an accepted Arm:

- the activation count is zero — the first post-arm edge is `Activation 1 of N`;
- no delay is pending, and a test delay that was pending never fires;
- no cooldown is running — detection is live at once;
- the detection latch and hold are clear;
- an AWAKE level asserted at arming is ignored until it drops and a new edge arrives.

This is proven by host tests (§4), not by review alone, and verified on the bench.

### 3.3 Invariants kept

- **The arm boolean is definitive**; the output is `armed && detectionMet && delay
  permits` at the single derivation point; `OutputSwitch` interlock unchanged.
- **Disarm order**: clear the boolean → re-derive the output (GPIOs off) → restart the
  test. The output is off before the accelerometer is touched.
- **Arm order**: restart (configure, confirm AWAKE clear) → set the boolean.
- **The stuck-AWAKE watchdog runs in both states**, because the part now runs in both.

## 4. Host-testable detection engine

The detection engine moves out of `App` into a pure class, `DetectionEngine`
(`src/detection_engine.{hpp,cpp}`), with hardware behind a small interface
(accelerometer configure/standby, PMIC cooldown timer, GRTC delay timer, fast-scan
request) and time passed in. `App` keeps the arm boolean, `DecideCommand()`, the output
derivation, `OutputSwitch`, LEDs and logging of commands.

The engine is host-tested with a fake hardware interface, covering at minimum: counting
and cooldown in both states; delay in both states (fast scan requested only when armed);
test restart from every sub-state; **every §3.2 arming case**; one-shot completion armed
vs. continuing disarmed; configure failure on arm; the watchdog in both states; and the
delayed-trigger hold.

Behaviour that is not changed by this amendment must be preserved exactly — the
extraction is a move, with the §3 differences on top.

## 5. App

The Settings page hint gains: **"To restart a test from zero (for example during a long
cooldown or delay), send these settings again, or Disarm from the Arm page."**
