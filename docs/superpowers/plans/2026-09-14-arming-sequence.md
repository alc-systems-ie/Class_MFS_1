# Arming Sequence and Fire-Pin Isolation Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Fire GPIOs have no software driver unless the device is Active; disarm disables them first; arming waits 10 s, restarts detection, enables the pins last, and fails safe to a warning on any step failure.

**Architecture:** A pure, host-tested `ArmingSequence` owns the Inactive/Arming/Active state and the step order, driving an `ArmingActions` interface implemented by `App`. `OutputSwitch` gains Disable/Enable and a boot pull-down check. `DecideCommand()` takes the three-valued state.

**Tech Stack:** C++20 / Zephyr NCS v3.2.4 (nRF54L05), host tests with g++ (`make test`), Flutter for one legend string.

**Spec:** `docs/superpowers/specs/2026-09-14-arming-sequence-amendment.md` (binding). Context: `docs/superpowers/specs/2026-09-14-disarmed-test-mode-amendment.md`, `docs/superpowers/specs/2026-09-14-command-types-amendment.md`, project `CLAUDE.md`.

## Global Constraints

- House style `~/.claude/CLAUDE.md`; clang-format `/Users/andy/nrfenv/bin/clang-format -i` on touched C++.
- **Fire pins disconnected (`GPIO_DISCONNECTED`) whenever not Active.** Boot: input-read low then disconnect; high → warning + switch latched faulty. Enable = `GPIO_OUTPUT_INACTIVE` with read-back (existing fallback if the input buffer is refused), verified low. Disable = drive both low, then disconnect both; always attempt both pins.
- `OutputSwitch::Set(false)` while disabled: no write, no verify, returns 0. `Set(true)` while disabled: refused `-EPERM`, latches faulty. Existing faulty-latch, interlock and series-MOSFET logging semantics unchanged.
- **Disarm order:** disable pins → state Inactive (cancel arming) → re-derive output (no engine tick) → restart the test disarmed.
- **Arm:** accepted → Arming; nothing for exactly `M_EXIT_DELAY_MS = 10000` ms (LED A nothing, LED B suppressed, scanner normal, engine test continues invisibly); then synchronously: restart detection armed → enable pins → Active + LED A Armed pattern. Any failure → disable pins, Inactive, restart disarmed, `signalWarning(reason, result)`, no LED A acknowledgement.
- **Arming commands:** Disarm cancels (disarm order, LED A slow flash); Arm and Settings do nothing (logged, no LED, no clock trim, no settings).
- `signalWarning` is a named stub: `LOG_ERR("WARNING (light TBC): %s (%d)!", reason, result)` only.
- The output derivation stays `armState == Active && DetectionMet && DelayPermitsFiring`, in `App::updateOutputState()` only; `IsOutputActive()` the only read. No engine tick or output derivation between the armed restart and setting Active.
- `ArmingSequence` compiles on the host with no Zephyr headers.
- Never flash, never touch the J-Link/RTT, never commit `credentials.conf` or `class_app/lib/services/bench_credentials.dart`; `class_app/lib/main.dart` carries a deliberate uncommitted bench edit — never stage or revert it.
- Commit trailer:
  ```
  Co-Authored-By: Claude Opus 5 (1M context) <noreply@anthropic.com>
  Claude-Session: https://claude.ai/code/session_01UySHgbzqJcwKqpRntC1PtN
  ```

---

### Task 1: `ArmingSequence`, three-valued `DecideCommand`, host tests

**Files:** Create `src/arming_sequence.hpp`, `src/arming_sequence.cpp`, `tests/test_arming_sequence.cpp`. Modify `src/arm_policy.hpp`, `tests/test_arm_policy.cpp`, `Makefile` (HOST_SRCS; host define if needed), `CMakeLists.txt` (target sources), `tests/test_main.cpp`. `src/app.*` only as far as needed to keep compiling with the new `DecideCommand` signature (Task 2 does the wiring).

**Interfaces (produce these names):**

```cpp
namespace alc
{
  enum class ArmState : uint8_t { Inactive = 0, Arming = 1, Active = 2 };   // moved out of App

  enum class ArmingStep : uint8_t { DisablePins, RestartDetection, EnablePins };

  class ArmingActions
  {
    public:
      virtual ~ArmingActions() = default;
      virtual int DisableFirePins() = 0;                 // drive low, disconnect
      virtual int RestartDetection(bool armed) = 0;      // DetectionEngine::Restart via App
      virtual int EnableFirePins() = 0;                  // output inactive, verified
      virtual void SignalWarning(ArmingStep step, int result) = 0;
  };

  class ArmingSequence
  {
    public:
      static constexpr int64_t M_EXIT_DELAY_MS { 10000 };
      explicit ArmingSequence(ArmingActions& actions);
      ArmState State() const;
      /** Inactive -> Arming, deadline now + M_EXIT_DELAY_MS. Ignored (returns false) unless Inactive. */
      bool BeginArming(int64_t nowMs);
      /** Service the exit delay; runs restart -> enable -> Active when due. Returns true on the tick it went Active. */
      bool Service(int64_t nowMs);
      /** From any state: DisableFirePins FIRST, then Inactive, then RestartDetection(false). Returns whether arming was cancelled. */
      bool Disarm();
      /** The last arming failure, if any since the last read. Cleared by the read. */
      bool TakeFailure(ArmingStep& step, int& result);
  };
}
```

`DecideCommand(ArmState state, bool fromNetworkManager, const protocol::Command&)`: Active unchanged (Disarm → Disarm; Arm/Settings → ReplayArmed); **Arming: Disarm → Disarm (trim yes), Arm/Settings → Ignore (no trim)**; Inactive unchanged. Add `ArmAction::Ignore` docs for the Arming case.

Design notes: `Disarm()` must call `DisableFirePins()` before changing state, even if the state is already Inactive, and must still complete (state Inactive, restart) if disabling fails — then `SignalWarning(DisablePins, result)`. `Service()` does nothing before the deadline, and on a restart or enable failure calls `DisableFirePins()`, sets Inactive, calls `RestartDetection(false)` and `SignalWarning(step, result)`. The enable step is never called if restart failed. The state is set Active only after `EnableFirePins()` returned 0.

- [ ] **Step 1: Failing tests** (`tests/test_arming_sequence.cpp`, a fake recording an ordered call log with configurable results):
  1. Arm: `BeginArming(t)`; `Service` at t, t+1, t+9999 → no calls, state Arming; at t+10000 → calls exactly `RestartDetection(true)`, `EnableFirePins()` in that order, state Active, `Service` returned true once.
  2. Disarm from Active: call log starts with `DisableFirePins`, then `RestartDetection(false)`; state Inactive.
  3. Disarm during Arming at t, t+5000, t+9999: `DisableFirePins` first; state Inactive; `Service` past t+10000 makes no restart(true)/enable calls; returns cancelled = true.
  4. Restart failure at the deadline: no `EnableFirePins`; `DisableFirePins`, `RestartDetection(false)`, `SignalWarning(RestartDetection, result)`; state Inactive.
  5. Enable failure: `DisableFirePins`, `RestartDetection(false)`, `SignalWarning(EnablePins, result)`; state Inactive; never Active at any point (assert state after each call via the fake observing `State()` inside callbacks).
  6. Disable failure during Disarm: state still Inactive, restart still called, warning raised with `DisablePins`.
  7. `BeginArming` while Arming or Active returns false and does not move the deadline.
  8. Disarm from Inactive still calls `DisableFirePins` first.
  Update `tests/test_arm_policy.cpp` for the three-valued state including the Arming rows (Disarm acts with trim; Arm and Settings Ignore with no trim/settings/mode).
- [ ] **Step 2:** `make test` fails (compile). Record.
- [ ] **Step 3:** Implement; adapt `src/app.*` minimally to the new `DecideCommand` signature and shared `ArmState` (map App's current two states; behaviour unchanged until Task 2).
- [ ] **Step 4:** `make test` green; firmware builds (`west build -b nrf54l15dk/nrf54l05/cpuapp -d build-task1 -p always -- -DEXTRA_CONF_FILE=credentials.conf`, delete after).
- [ ] **Step 5:** clang-format; commit `Add the host-tested arming sequence and the Arming command policy`.

---

### Task 2: `OutputSwitch` isolation and `App` wiring

**Files:** Modify `src/output_switch.hpp`, `src/output_switch.cpp`, `src/app.hpp`, `src/app.cpp`; `boards/nrf54l15dk_nrf54l05_cpuapp.overlay` comment only if it states the pins are driven low early in boot.

- [ ] **Step 1: `OutputSwitch`.** `Init()` → the boot check (gpio ready; each pin `GPIO_INPUT` read raw, expect 0; then `GPIO_DISCONNECTED`; a high or an error → fault latch; return negative). Add `int Enable()` (configure `GPIO_OUTPUT_INACTIVE | GPIO_INPUT`, fallback output-only as today, drive low, verify low; refuse `-EPERM` if faulted; on failure disable and latch faulty) and `int Disable()` (drive both low, then `GPIO_DISCONNECTED` both; attempt both pins at every step; `m_enabled = false` regardless; negative on any failure with the existing one-versus-both logging). `bool IsEnabled() const`. `Set()`/`ForceSafe()` per Global Constraints for the disabled case. Update the class doc comment: pins have no driver unless enabled; external pull-downs; boot check. Update the log line "Fire output initialised, de-energised and verified" to state the pins are isolated and read low.
- [ ] **Step 2: `App` implements `ArmingActions`** (private): `DisableFirePins` → `m_output_switch.Disable()`; `RestartDetection(armed)` → existing `restartEngine(armed)`; `EnableFirePins` → `m_output_switch.Enable()`; `SignalWarning(step, result)` → `signalWarning(stepName, result)` stub per Global Constraints. `m_arm_state` replaced by `m_arming.State()` (declare `ArmingSequence m_arming` after the members its actions touch). If boot `Init()` fails, keep booting (commands still work) but raise the warning — the switch is latched faulty so arming will fail safe; log it clearly. (Today boot aborts on Init failure: change to continue, because an isolated faulty switch is safe and the device remains diagnosable; note it in the report.)
- [ ] **Step 3: `setArmState` replaced.** Every disarm path (command, trigger latch, boot) calls `m_arming.Disarm()`. `ArmingSequence::Disarm()` runs DisableFirePins → state Inactive → `RestartDetection(false)`; App's `RestartDetection(false)` implementation FIRST re-derives the output with no engine tick (`updateOutputState(EngineTick::Skip)` — state is already Inactive, so the output is false) and THEN restarts the engine. That realises amendment §2 exactly. `RestartDetection(true)` just restarts the engine armed (no re-derivation; the state is still Arming so the output stays false). Arm command → `m_arming.BeginArming(now)` (no LED A). Main loop, before `updateOutputState()`: `if (m_arming.Service(now)) { playLedPattern(LedPattern::Armed); }`; after it, `TakeFailure` → log the failing step (no LED A; the warning stub was already called by the sequence). Boot: `m_arming.Disarm()` after `Init()` isolated the pins.
- [ ] **Step 4: Output and LEDs.** Derivation uses `m_arming.State() == ArmState::Active`. LED B = `m_engine.DetectionMet()` except `false` while Arming. The `Detection met/cleared` log follows LED B. LED A bench idle level: lit while Inactive as today; dark while Arming (nothing shown).
- [ ] **Step 5: `applyCommand`.** Pass `m_arming.State()` to `DecideCommand`. Arming + Disarm → cancel arming via the disarm path, LED A `Disarmed`. `Ignore` during Arming → `LOG_INF("Arming: %s from slot %u n %u ignored - only a disarm is accepted while arming.", …)`, no LED. Arm while Inactive → `BeginArming` and `LOG_INF("Arming: fire pins isolated, arming in %u s.", …)`. Disarm pattern: `DisarmedDelayCancelled` only if an armed delay was pending (unchanged).
- [ ] **Step 6:** Grep `src/` and the overlay for comments claiming the fire pins are driven low from boot / configured at Init and fix them. `make test` green; firmware builds with zero warnings from touched files; delete build dir. clang-format; commit `Isolate the fire pins unless armed; arm through a 10 s exit delay and fail safe`.

---

### Task 3: Docs, bench checklist, app legend

**Files:** `CLAUDE.md` (project), `docs/superpowers/specs/2026-09-14-disarmed-test-mode-amendment.md` (§3.3 pointer), `docs/superpowers/specs/2026-09-14-command-types-amendment.md` (§2.2 pointer), `docs/superpowers/specs/2026-09-12-app-control-design.md` (§6.2/§6.4/§6.7 banners where they state pin or arm behaviour), `docs/superpowers/plans/2026-09-13-bench-checklist.md`; in `/Users/andy/nordic/ncs/v3.2.4/class_app`: `lib/devices/mfs1/mfs1_screen.dart` legend, `test/mfs1_flow_test.dart` if it asserts the legend.

- [ ] **Step 1: `CLAUDE.md`.** Under the arm invariant add: "**Fire pins are isolated unless armed** — `GPIO_DISCONNECTED` with external pull-downs; disarm disables them first; arming waits 10 s, restarts detection, enables the pins last, and any step failure fails safe to the warning (light TBC) (`docs/superpowers/specs/2026-09-14-arming-sequence-amendment.md`)." Update the disarm/arm order text to the Global Constraints orders and the Access rules bullet to include the Arming state (only Disarm acts).
- [ ] **Step 2:** Pointers/banners in the listed specs; fix statements that present the old disarm order or pins-driven-from-boot as current.
- [ ] **Step 3: Bench checklist** `## 5c. Arming sequence and fire-pin isolation (plan 2026-09-14)` before §9, using exact RTT strings from `src/app.cpp` / `src/output_switch.cpp`:
  1. Boot log shows the pins isolated and read low.
  2. Arm: nothing on LED A or LED B for 10 s (tap during it: no LED B); then rapid flash; RTT arming lines; measure fire lines with a meter: floating/pulled low before, driven low after arming.
  3. Disarm during the 10 s: slow flash, never armed, fire lines stay isolated.
  4. Arm/Settings during the 10 s: nothing happens (RTT "ignored" line).
  5. Disarm while armed: fire lines isolated again (meter), slow flash.
  6. Armed trigger: fires, latches Inactive, fire lines isolated afterwards.
  7. Failure path (if safely inducible, e.g. hold the ADXL367 unresponsive, or skip): warning log, Inactive, no rapid flash.
- [ ] **Step 4: App legend.** In the Arm page LED legend replace "Rapid flash: armed (also shown if it was already armed)." with "Rapid flash: armed — about 10 s after the Send is heard, once the exit delay has passed (also shown if it was already armed)." and add "Nothing for 10 s after arming is normal: leave the area." Update any widget test asserting the legend. `flutter test -j 1`, `flutter analyze`. Commit in class_app with only those files staged: `Explain the 10 s exit delay on the Arm page`.
- [ ] **Step 5:** Commit firmware docs: `Docs: arming sequence and fire-pin isolation`.
