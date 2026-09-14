# MFS_1 Arming Sequence and Fire-Pin Isolation — Design Amendment

**Date:** 2026-09-14 (bench session 1)
**Amends:** `docs/superpowers/specs/2026-09-14-disarmed-test-mode-amendment.md` §3, §3.3;
`docs/superpowers/specs/2026-09-14-command-types-amendment.md` §2.2; the `OutputSwitch`
contract in `src/output_switch.hpp` (pins configured from boot). Where this document and
those disagree, **this document wins**.

Owner requirements, 2026-09-14, with the recommendations the owner accepted.

## 1. Fire pins are isolated whenever the device is not armed

The fire gates carry **external pull-downs**. While the device is not Active the fire
GPIOs have **no software driver**: `gpio_pin_configure_dt(pin, GPIO_DISCONNECTED)` —
input and output buffers both disconnected, so only the resistors hold the lines.

| Moment | Fire pins |
|---|---|
| Reset → first firmware action | untouched (pull-downs hold them) |
| Boot check | each pin briefly configured as a plain input (no pull), read — **must read low** — then disconnected. A high line is logged, raises the warning (§4) and latches the switch faulty, so the device can never arm |
| Inactive, Arming | disconnected |
| Last step of arming | configured `GPIO_OUTPUT_INACTIVE` with read-back, verified low |
| Active | outputs, driven only by `OutputSwitch::Set()` from the single derivation point |
| Disarm, trigger latch, any failure | **first action:** both driven low, then disconnected |
| Fire switch fault while Active (failed assert, failed clear or clear read-back not low) | isolated at once by the latch, then the ordinary disarm (§4.1) |

"Disable" drives both lines low for an instant before disconnecting, so a line that was
high is emptied at once rather than at the resistors' RC rate.

`OutputSwitch::Set(false)` on a disabled switch does nothing and verifies nothing (the
input buffer is disconnected, so a read-back would read garbage and falsely latch a
fault). `Set(true)` on a disabled switch is refused (`-EPERM`) and latches faulty, as a
bypassed derivation point is a bug.

## 2. Disarm order

1. **Disable the fire pins** (drive low, disconnect).
2. Clear the arm state (Inactive), cancel any arming in progress.
3. Re-derive the output through the single derivation point (it is now false).
4. Restart the detection test from zero.

The one-shot trigger latch (armed trigger complete → Inactive) and the fire switch fault
fail-safe (§4.1) use this same order.
This replaces the disarmed-test-mode amendment §3.3 disarm order, which cleared the
boolean first: the pins now go safe before any state changes.

## 3. The arming sequence

**Amended 2026-09-14 — scan reliability:**
`docs/superpowers/specs/2026-09-14-scan-reliability-amendment.md` §3 supersedes
the scanner cadence bullet below — the scanner runs continuously for the whole
10 s exit delay, not at its normal duty-cycled cadence, so a Disarm sent during
the delay is heard within a fraction of a second.

A new arm state, **Arming**, sits between Inactive and Active.

1. **Arm accepted** (persisted before acting, as for every command) → **Arming**. The fire
   pins stay disconnected. **Nothing else happens for 10 s** (`M_EXIT_DELAY_MS`), so the
   engineer can vacate the area:
   - LED A shows nothing (no acknowledgement yet);
   - **LED B is suppressed** — nothing visible while the engineer walks away;
   - the main loop keeps running and the scanner switches to its continuous
     cadence (`Scan cadence now CONTINUOUS.`), returning to duty-cycled when
     Arming ends;
   - the detection engine keeps its disarmed test running (invisible); its state is
     discarded at step 2.
2. After 10 s, in one synchronous step, in order:
   1. **Restart detection armed** — the engine's fresh session: accelerometer configured
      through the loop-mode bootstrap, AWAKE confirmed clear, all counts, cooldown and
      delay discarded (disarmed-test-mode amendment §3.2).
   2. **Enable the fire pins** — `GPIO_OUTPUT_INACTIVE`, read back low. **This is the last
      step.**
   3. **Only if every step succeeded:** state **Active**, LED A plays the Armed rapid
      flash.
3. **Any step fails** → fail safe: disable the fire pins, state Inactive, restart the test
   disarmed, **raise the warning** (§4), log the failing step and its result. No LED A
   acknowledgement.

No tick of the detection engine and no output derivation occurs between step 2.1 and
2.3, so the engine's armed session can never drive an output before the pins are
enabled and the boolean is set.

### 3.1 Commands during Arming

**Arming is treated as armed for command policy, except that nothing is replayed:**

| State | Command | Action |
|---|---|---|
| Arming | Disarm | **Cancel arming**: §2 disarm order; LED A slow flash |
| Arming | Arm | Nothing (logged) |
| Arming | Settings | Nothing (logged) |

`DecideCommand()` stays the single decision point and takes the three-valued state.

### 3.2 Unchanged

Active behaviour (except that a fire switch fault now disarms — §4.1), ReplayArmed for
Arm/Settings while **Active**, the one-shot, the delay
interlock, the single output derivation and `IsOutputActive()` as the only sanctioned
read are unchanged. The command's minute still trims the clock only as before (an Arm
accepted while Inactive trims; ignored commands during Arming do not).

## 4. Warning light — TBC, interim on LED B

The owner will choose a dedicated LED. `App::signalWarning(reason, result)` is the one
warning path, so wiring that pin later is a one-place change. It logs
`LOG_ERR("WARNING (light TBC): %s (%d)!")` and, **until the dedicated light is chosen,
plays an interim warning on LED B**:

- **Pattern:** three long pulses — 700 ms on / 300 ms off for 3 s — framed by the same
  300 ms dark gap before and after as every LED A pattern (`LedPattern::Warning`).
- **All builds**, production included: production has no other indicator.
- **Ownership:** while it plays, the warning's own 10 ms timer is LED B's only writer and
  it overrides the bench detection level; the main loop writes LED B again only once it
  has ended (the same single-writer rule LED A's sequencer and the loop follow). A new
  warning replaces one already playing. A warning raised before the LEDs are initialised
  (the boot pin check) starts rendering once they are, still inside its dark lead-in.
- **LED A shows nothing** for an arming failure — no acknowledgement of any kind.

It is raised by:

1. an arming-step failure (§3 step 3);
2. a boot pin check reading high, or not completing (§1);
3. a fire-pin disable failure;
4. **a fire switch failure while Active** — `OutputSwitch::Set()` returning an error from
   the single derivation point (a failed write or read-back, a refused assert) — **raises
   the warning and fails safe to disarmed** (§4.1). Raised at most once per Active session.
5. **the command scanner not running while Active** — `CommandScanner::IsScanning()`
   false (a start and its fallback both failed) — **raises the warning** with reason
   `scanner not running while armed - a disarm could not be heard` (`-ENODEV`) **and
   fails safe to disarmed** (§4.2).

### 4.1 Fire switch faults (owner decisions, 2026-09-14)

**(a) A failed clear latches.** `Set(false)` on an enabled switch fails if either pin's
write fails **or** its read-back is not low. Either way the switch goes safe exactly as
every other `OutputSwitch` failure does: both pins driven low then disconnected
(`enterFaultState()`), the switch **latched faulty**, and the failure logged once. Both
gates failing — both writes, or both reading back high — is the emergency
`BOTH FIRE GATES FAILED TO CLEAR … THE DEVICE MAY BE FIRING!`; one gate is
`Fire gate N failed to clear … redundancy is LOST!`. The latched switch is disabled, so
later `Set(false)` calls return 0 without logging again.

**(b) A switch fault while Active disarms.** When `Set()` returns an error while Active,
`App::updateOutputState()` raises the warning and flags the fault. The main loop, at the
same point as the one-shot trigger latch (the disarm path re-enters
`updateOutputState()`), logs `Fire switch fault while armed - disarmed (fail safe).` and
runs the ordinary disarm (§2): pins disabled first, Inactive, the test restarted. **No
LED A acknowledgement.** The switch stays latched faulty, so a later Arm fails at the
enable step (§3 step 3) and raises its own arming-failure warning (source 1) — always,
independent of source 4.

### 4.2 Scanner loss while Active (owner rule, 2026-09-14: always fail safe)

A device that cannot scan cannot hear a disarm, so it must not stay armed — idle or
with a trigger delay pending. Every main-loop tick, after every scanner call (command
handling, `serviceScanHealth()`, the arming service, the cadence gate) and **before**
the output is derived, `App::Run()` checks `Active && !IsScanning()`. If so it raises
the warning (source 5), runs the ordinary disarm (§2) — pins disabled first, Inactive,
any pending trigger delay cancelled, the test restarted — and logs
`Scanner not running while armed - disarmed (fail safe)!`. **No LED A
acknowledgement.** Once per event: the disarm ends the Active state. A scan running at
the fallback cadence still hears a disarm and is not a loss. The loop is entered only
after `CommandScanner::Start()` succeeds, so the check never sees a scanner that was
never started. While Arming, a lost scanner is caught by the scanner check at the end
of the exit delay, which refuses the arm (§3).

As a defensive guard behind that disarm, the detection engine never fires an ARMED
delay that ran without a scanner: at expiry it sets no detection and no hold and
reports `DelayExpiredScanLostSuppressed`, logged as
`Trigger suppressed: the scanner was not running during the delay (fail safe)!`. A
disarmed test delay is unaffected. This supersedes the 2026-09-13 "prioritise fire"
ruling.

## 5. Host-testable arming sequence

The sequence lives in a pure class, `ArmingSequence` (`src/arming_sequence.{hpp,cpp}`),
driving an abstract `ArmingActions` interface (disable pins, restart detection, enable
pins, signal warning) with time passed in. Host tests cover at minimum: disarm calls
disable-pins before anything else; arm → nothing for 10 s (no restart, no enable) then
restart → enable → Active in that order; Disarm at 0 s, mid-delay and at the last tick
before 10 s cancels with pins disabled and never reaches restart/enable; a failure at
restart and at enable each end Inactive with pins disabled, the warning raised and no
Active; Arm/Settings during Arming change nothing; state is never Active before enable
succeeds. `DecideCommand()` tests gain the Arming rows.
