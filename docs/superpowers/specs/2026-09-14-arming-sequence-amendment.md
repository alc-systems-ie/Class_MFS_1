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

The one-shot trigger latch (armed trigger complete → Inactive) uses this same order.
This replaces the disarmed-test-mode amendment §3.3 disarm order, which cleared the
boolean first: the pins now go safe before any state changes.

## 3. The arming sequence

A new arm state, **Arming**, sits between Inactive and Active.

1. **Arm accepted** (persisted before acting, as for every command) → **Arming**. The fire
   pins stay disconnected. **Nothing else happens for 10 s** (`M_EXIT_DELAY_MS`), so the
   engineer can vacate the area:
   - LED A shows nothing (no acknowledgement yet);
   - **LED B is suppressed** — nothing visible while the engineer walks away;
   - the main loop keeps running and the scanner keeps its normal cadence;
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

Active behaviour, ReplayArmed for Arm/Settings while **Active**, the one-shot, the delay
interlock, the single output derivation and `IsOutputActive()` as the only sanctioned
read are unchanged. The command's minute still trims the clock only as before (an Arm
accepted while Inactive trims; ignored commands during Arming do not).

## 4. Warning light — TBC

The owner will choose a dedicated LED. Until then `App::signalWarning(reason, result)` is
a **named stub**: it logs `LOG_ERR("WARNING (light TBC): %s (%d)!")` and does nothing
else, so wiring it to a pin later is a one-place change. It is raised by: an arming-step
failure, a boot pin check reading high, and a fire-pin disable failure.

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
