# Disarmed Test Mode Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** The detection engine runs identically armed and disarmed (LED B vs. fire GPIOs being the visible difference), Disarm restarts a test from zero, and arming mid-test is proven safe by host tests.

**Architecture:** Move the detection engine out of `App` into a pure, host-tested `DetectionEngine` class with hardware behind an abstract interface and time passed in; `App` keeps the arm boolean, command policy, the single output derivation, `OutputSwitch` and LEDs, and drives the engine. Then change the engine's behaviour per the amendment and wire it in.

**Tech Stack:** C++20 / Zephyr NCS v3.2.4 (nRF54L05), host tests with g++ (`make test`), Flutter for one hint string.

**Spec:** `docs/superpowers/specs/2026-09-14-disarmed-test-mode-amendment.md` (binding). Context: `docs/superpowers/specs/2026-09-14-command-types-amendment.md`, `docs/superpowers/specs/2026-09-12-app-control-design.md` §6.2–§6.5.1, project `CLAUDE.md` (arm invariant, edge-triggered arming, ADXL367 loop-mode bootstrap).

## Global Constraints

- House style `~/.claude/CLAUDE.md` (C++20, `alc` namespace, `enum class`, `M_`/`m_`/`s_` prefixes, camelCase locals, PascalCase public methods, no literals in calls, locals declared at the top, `LOG_ERR` ends with `!`). clang-format `/Users/andy/nrfenv/bin/clang-format -i` on every touched C++ file.
- **The arm boolean is definitive.** Output = `armed && detectionMet && delayPermitsFiring` derived ONLY in `App::updateOutputState()`; `OutputSwitch` and its interlock unchanged; `IsOutputActive()` the only sanctioned read.
- **Disarm order:** boolean Inactive → re-derive output (GPIOs off) → restart the test. **Arm order:** restart (configure, confirm AWAKE clear) → boolean Active only on success.
- **Restart from zero** clears: activation count, detection latch and hold, previous-AWAKE witness, cooldown (PMIC timer stopped, expired latch, retry state), pending delay (timer stopped), one-shot flags, watchdog ticks; then reconfigures the ADXL367 via the loop-mode bootstrap and sets the stale-AWAKE suppression from the post-configure AWAKE read.
- **Delay runs in both states** on the GRTC `k_timer`. **Fast scan and the PM lock only while armed.** A test delay logs as a test, never `TRIGGER PENDING`.
- **LED B = detection in both states** (bench builds). LED A double blink on Disarm only if an ARMED delay was pending.
- **Stuck-AWAKE watchdog in both states.**
- **One-shot:** armed trigger completion latches Inactive (restart); disarmed completion carries on.
- **Preserve every existing behaviour not changed by the amendment** — the delayed-trigger hold (`M_DELAYED_TRIGGER_HOLD_MS`), cooldown deadline/tolerance/grace and 1 Hz re-arm retry, "fire even if the scanner was lost during an armed delay" warning, no counting while cooldown/delay/trigger-in-progress/stale-suppression, N = 1 ignores cooldown.
- Host test flags `-std=c++20 -Wall -Wextra -Wpedantic -Werror`; `DetectionEngine` must compile on the host with no Zephyr headers.
- Never flash, never touch the J-Link/RTT, never commit `credentials.conf` or `class_app/lib/services/bench_credentials.dart`; `class_app/lib/main.dart` has a deliberate uncommitted bench edit (engineer slot 2) — never stage or revert it.
- Commit trailer:
  ```
  Co-Authored-By: Claude Opus 5 (1M context) <noreply@anthropic.com>
  Claude-Session: https://claude.ai/code/session_01UySHgbzqJcwKqpRntC1PtN
  ```

---

### Task 1: `DetectionEngine` — pure class and host tests

**Files:**
- Create: `src/detection_engine.hpp`, `src/detection_engine.cpp`, `tests/test_detection_engine.cpp`
- Modify: `Makefile` (add `src/detection_engine.cpp` to `HOST_SRCS`), `tests/test_main.cpp` (register the suite)
- Read (do not modify in this task): `src/app.cpp` `enableAccelerometer`, `disableAccelerometer`, `updateOutputState`, `setArmState`, `beginCooldown`, `serviceCooldown`, `delayPermitsFiring`, `beginDelay`, `cancelDelay`, `serviceScanHealth`, the `m_trigger_complete` handling in `Run()`, and the Arm/Tune/Disarm cases of `applyCommand`; `src/app.hpp` members and constants.

**Interfaces (produce exactly these names; parameters may gain `const`/refs as house style requires):**

```cpp
namespace alc
{
  /** Hardware the engine drives. App implements it over Adxl367, Npm2100, k_timer and CommandScanner. */
  class DetectionHardware
  {
    public:
      virtual ~DetectionHardware() = default;
      /** Loop-mode bootstrap at the threshold, then read AWAKE from STATUS. Negative errno on failure. */
      virtual int ConfigureAccelerometer(uint16_t thresholdLsb, bool& awake) = 0;
      virtual int StandbyAccelerometer() = 0;
      /** Stop, set general-purpose mode, set duration, clear the event, start. Negative errno on failure. */
      virtual int StartCooldownTimer(uint32_t durationMs) = 0;
      virtual int StopCooldownTimer() = 0;
      virtual int CooldownTimerExpired(bool& expired) = 0;
      virtual int ClearCooldownTimerEvent() = 0;
      virtual void StartDelayTimer(uint32_t durationMs) = 0;
      virtual void StopDelayTimer() = 0;
      /** True while the GRTC delay timer still has time remaining. */
      virtual bool DelayTimerRunning() const = 0;
      /** Continuous scan + PM lock while true. Engine calls it only for ARMED delays. */
      virtual int SetTriggerPendingScan(bool fast) = 0;
      virtual bool ScannerRunning() const = 0;
  };

  struct DetectionSettings
  {
      uint8_t activations;
      uint16_t cooldownSeconds;
      uint16_t delaySeconds;
      uint16_t thresholdLsb;
  };

  class DetectionEngine
  {
    public:
      explicit DetectionEngine(DetectionHardware& hardware);

      /** Restart from zero (amendment section 3). Returns the configure result; on failure the 1 Hz re-arm retry is pending. */
      int Restart(const DetectionSettings& settings, bool armed, int64_t nowMs);

      /** One 100 ms loop tick. awake is INT1's logical level. */
      void Tick(const DetectionSettings& settings, bool armed, bool awake, int64_t nowMs);

      bool DetectionMet() const;
      bool DelayPermitsFiring() const;      // !delayPending && !hardware.DelayTimerRunning()
      bool DelayPending() const;
      bool DelayPendingArmed() const;       // a pending delay that started while armed
      uint8_t ActivationCount() const;
      bool InCooldown() const;
      bool IgnoringStaleAwake() const;

      /** The output was asserted this session and has now ended. App latches Inactive when armed. Cleared by the read. */
      bool TakeTriggerComplete();

      /** App reports whether the output actually asserted this tick (armed one-shot tracking). */
      void NoteOutput(bool outputActive);
  };
}
```

Design notes the implementer must honour:
- The engine logs through Zephyr `LOG_*` on target only if that is trivially guarded; simplest is **no logging inside the engine** and small query methods/return values that let `App` log the same messages it logs today (e.g. `Tick` returns or exposes an event enum: `None, Activation, CooldownStarted, CooldownElapsed, CooldownForced, RearmFailed, DelayStarted, DelayExpired, DelayExpiredScanLost, WatchdogRearm, Detection...`). Choose one mechanism and document it in the header.
- The one-shot: today `m_trigger_fired` is set when the OUTPUT asserts and completion is when it deasserts. Armed behaviour must stay exactly that (via `NoteOutput`). Disarmed: a detection period in progress blocks counting exactly as an armed trigger period does (amendment §2: identical except outputs) — track it on `DetectionMet()` while disarmed; disarmed completion does NOT request a latch.
- Scan-lost during an ARMED delay: preserve the current semantics (`serviceScanHealth` sets it each tick while pending and the scanner is down; expiry still fires and reports the loss).
  **Superseded 2026-09-14 (always fail safe):** an armed delay that ran without a scanner no longer fires — the engine suppresses it (`DelayExpiredScanLostSuppressed`) and App fails safe to disarmed on scanner loss while arming or armed. See `docs/superpowers/specs/2026-09-14-arming-sequence-amendment.md` §4.2.
- Cooldown timer failure and re-arm retry: preserve `beginCooldown` / `serviceCooldown` semantics exactly (fail toward detecting; deadline = duration + duration/`M_COOLDOWN_TOLERANCE_DIVISOR` + `M_COOLDOWN_GRACE_MS`; retry every `M_COOLDOWN_RETRY_MS`). Move those constants into the engine.
- Restart when configure fails: leave the part stood down, and arm the retry path so `Tick` keeps retrying at 1 Hz; `Restart` returns the negative result so `App` can refuse arming.

- [ ] **Step 1: Write the fake and the failing tests** in `tests/test_detection_engine.cpp`. A `FakeHardware` records calls, lets tests set configure results/AWAKE-after-configure, cooldown expiry, delay-timer running, scanner running. Required cases (each its own block with a comment naming the amendment clause):
  1. Counting: N = 3, cooldown 0 — three rising edges → DetectionMet; levels don't double-count; count resets to 0 at detection. Both `armed = false` and `armed = true`.
  2. Cooldown between activations (N = 3, 8 s) in both states: StandbyAccelerometer + StartCooldownTimer(8000); no counting during cooldown; expiry via fake PMIC → reconfigure → counting resumes; deadline fallback when the PMIC never reports; N = 1 never starts a cooldown.
  3. Delay in both states (delay 30 s): detection waits for the delay; `SetTriggerPendingScan(true)` called only when armed, never when disarmed; hold keeps DetectionMet for `M_DELAYED_TRIGGER_HOLD_MS` after expiry even with AWAKE clear; `DelayPendingArmed()` true only for the armed delay.
  4. Restart from zero from each sub-state (part-counted, mid-cooldown, mid-delay, during detection/hold, stale suppression active): afterwards count 0, !InCooldown (StopCooldownTimer called if it was running), !DelayPending (StopDelayTimer called), !DetectionMet, and configure called once.
  5. **SAFETY — arming mid-test.** Start disarmed, drive into each of: 2 of 3 counted; mid-cooldown; mid test-delay; test detection period (DetectionMet true); AWAKE asserted with configure reporting AWAKE still set. Then `Restart(settings, true, now)`. Assert per amendment §3.2: count 0; a single new edge gives count 1 not detection; no pending delay and the old test delay never produces DetectionMet even after its original deadline passes; no cooldown; DetectionMet false; with AWAKE asserted at arming, further ticks with AWAKE held produce no activation until AWAKE drops and a fresh edge arrives. For each, also simulate App's derivation `armed && DetectionMet() && DelayPermitsFiring()` over the next 20 ticks with AWAKE toggling only as specified and assert it never becomes true when it should not.
  6. Configure failure on `Restart(..., true, ...)`: returns negative; `Tick` retries configure no more than once per `M_COOLDOWN_RETRY_MS`; success later resumes detection.
  7. One-shot: armed — `NoteOutput(true)` then `NoteOutput(false)` → `TakeTriggerComplete()` true once; disarmed — detection period end → `TakeTriggerComplete()` false, counting resumes afterwards; no counting during either period.
  8. Watchdog: AWAKE held for `M_AWAKE_STUCK_TICKS` → reconfigure, DetectionMet cleared, stale suppression kept — in BOTH states; configure failure hands over to the retry path.
  9. Scan lost during an armed delay: `ScannerRunning()` false during the delay → expiry still gives DetectionMet and reports the scan-lost event.
- [ ] **Step 2:** `make test` fails to compile (no engine). Record the output.
- [ ] **Step 3:** Implement `src/detection_engine.{hpp,cpp}` by MOVING the logic from `App` (copy now; `App` is rewired in Task 2), applying the amendment differences. Carry the existing safety comments across with the code they explain.
- [ ] **Step 4:** `make test` — all suites pass, output pristine. Firmware still builds (`west build -b nrf54l15dk/nrf54l05/cpuapp -d build-task1 -p always -- -DEXTRA_CONF_FILE=credentials.conf`; add `src/detection_engine.cpp` to `CMakeLists.txt` target sources if sources are listed explicitly); delete `build-task1`.
- [ ] **Step 5:** clang-format; commit `Extract the detection engine into a host-tested class`.

---

### Task 2: Wire `App` to `DetectionEngine`

**Files:** Modify `src/app.hpp`, `src/app.cpp`; `CMakeLists.txt` if needed.

**Interfaces:** Consumes Task 1's `DetectionHardware`, `DetectionSettings`, `DetectionEngine` exactly as committed (read the header).

- [ ] **Step 1:** `App` implements `DetectionHardware` (private inheritance or a small private adapter member) over `m_accelerometer` (`ConfigureLoopMode` with the existing Kconfig parameters + `ReadAwake`; `Standby`), `m_pmic` timer calls, `m_delay_timer`, and `m_scanner` (`SetFastScan` + the PM lock get/put moved into `SetTriggerPendingScan`; `IsScanning`). `serviceScanHealth()` keeps its retry/cadence role; scan-lost tracking moves into the engine or is fed by it — no duplicate state.
- [ ] **Step 2:** Remove the engine state and methods from `App` that moved (`m_detection_met`, `m_activation_count`, cooldown/delay/hold/trigger/watchdog/stale members, `beginCooldown`, `serviceCooldown`, `beginDelay`, `cancelDelay`, `enableAccelerometer`/`disableAccelerometer` as far as replaced). `delayPermitsFiring()`/`interlockThunk` delegate to the engine.
- [ ] **Step 3:** `updateOutputState()`: `m_engine.Tick(...)`, then `m_output_active = (m_arm_state == ArmState::Active) && m_engine.DetectionMet() && m_engine.DelayPermitsFiring();`, `m_output_switch.Set(m_output_active);`, `m_engine.NoteOutput(m_output_active);`. Keep the invariant banner comment. Log the engine's events with today's message texts (test-delay wording per Global Constraints).
- [ ] **Step 4:** `setArmState(Active)`: `result = m_engine.Restart(settings, true, now)`; on failure log and return (Inactive stays; do NOT stand the part down permanently — the engine's retry keeps the test alive); on success `m_arm_state = Active`. `setArmState(Inactive)`: `m_arm_state = Inactive` → `updateOutputState()`-equivalent output re-derivation (GPIOs off) → `m_engine.Restart(settings, false, now)`. Keep and update the EDGE-TRIGGERED ARMING comment: the mechanism is now "reconfigure on every restart", not "standby while deactivated".
- [ ] **Step 5:** `applyCommand`: Disarm pattern = `DisarmedDelayCancelled` only if `m_engine.DelayPendingArmed()` before disarming; Tune restarts via `m_engine.Restart(settings, false, now)` (replace the ad-hoc clears); Arm keeps its path through `setArmState(Active)` (remove the ad-hoc clears now owned by Restart). Main loop: `if (m_engine.TakeTriggerComplete() && m_arm_state == ArmState::Active)` → the existing latch log + `setArmState(Inactive)`.
- [ ] **Step 6:** Boot: `setArmState(ArmState::Inactive)` now starts testing. Grep `src/` for stale comments claiming the ADXL is held in standby while deactivated and fix them.
- [ ] **Step 7:** `make test` passes; firmware builds with no warnings from `src/app.cpp` or `src/detection_engine.cpp`; delete the build dir. clang-format; commit `Run the detection engine in both arm states; Disarm restarts the test`.

---

### Task 3: Docs, bench checklist and the app hint

**Files:** `CLAUDE.md` (project), `docs/v1-scope.md` §1.0.1 note, `docs/superpowers/specs/2026-09-12-app-control-design.md` §6.3 and §6.5.1 (banner + current behaviour), `docs/superpowers/specs/2026-09-14-command-types-amendment.md` §2.2 Inactive rows (pointer), `docs/superpowers/plans/2026-09-13-bench-checklist.md`; in `/Users/andy/nordic/ncs/v3.2.4/class_app`: `lib/devices/mfs1/mfs1_settings_screen.dart`, `test/mfs1_flow_test.dart`.

- [ ] **Step 1: `CLAUDE.md`.** In the "arming is edge-triggered" paragraph replace "holds the ADXL367 in standby while deactivated and configures it afresh on activation" with "reconfigures the ADXL367 afresh on every arm — and on every test restart — so there is no stale level to inherit", keep the load-bearing order text updated to the Global Constraints orders, and add a bullet under the arm invariant: "**Disarmed differs from armed in exactly two ways** — parameters change only while disarmed, and a trigger shows on LED B instead of the fire GPIOs. Detection, cooldown and delay run identically; Disarm restarts a test from zero; arming always starts a fresh session (`docs/superpowers/specs/2026-09-14-disarmed-test-mode-amendment.md`)."
- [ ] **Step 2:** v1-scope §1.0.1 and design spec §6.3/§6.5.1: add the "Amended 2026-09-14 — disarmed test mode: …amendment.md supersedes this section where they disagree." banner and correct statements presenting standby-while-disarmed or no-delay-while-disarmed as current.
- [ ] **Step 3: Bench checklist.** Add `## 5b. Disarmed test mode (plan 2026-09-14)` before §9 with checkbox steps (RTT expectations using the engine's log texts as implemented — read `src/app.cpp` for exact strings):
  1. Boot/flash without `--recover`, provision: tap three times with no command sent → LED B lights on the third (settings as stored).
  2. Disarm → count restarts (next tap is `Activation 1 of N`).
  3. Settings with a 30 s delay, 1 activation → tap: LED B lights only after ~30 s; RTT shows the TEST delay wording; no continuous scan logged.
  4. Long cooldown (e.g. N = 2, 10 min): tap once, then Disarm → next tap is `Activation 1 of 2` immediately, no wait.
  5. **SAFETY, arm mid-test** (N = 3, cooldown 8 s, delay 0): (a) two taps, Arm, one tap → no fire, RTT `Activation 1 of 3`; (b) one tap (cooldown running), Arm immediately, tap → `Activation 1 of 3`, no fire; (c) set delay 30 s, N = 1, tap (test delay pending), Arm within 30 s, leave untouched past 30 s → no fire; (d) shake the device continuously while sending Arm → no fire until motion stops and a fresh tap completes the count.
  6. Armed trigger → fires once, latches Inactive, and the test resumes (LED B on the next count).
- [ ] **Step 4: App hint.** Append to the Settings page hint text (the `'One blink: settings applied…'` string): `' To restart a test from zero (for example during a long cooldown or delay), send these settings again, or Disarm from the Arm page.'` Add a `find.textContaining('To restart a test from zero')` assertion to an existing Settings-page widget test. `flutter test -j 1`, `flutter analyze`. Commit in class_app with ONLY those two files staged (`git add lib/devices/mfs1/mfs1_settings_screen.dart test/mfs1_flow_test.dart`): `Tell the engineer how to restart a test`.
- [ ] **Step 5:** Commit firmware docs: `Docs: disarmed test mode in CLAUDE.md, v1-scope, design spec and the bench checklist`.
