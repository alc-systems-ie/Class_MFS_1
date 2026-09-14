#include <cassert>
#include <cerrno>
#include <cstdio>
#include <vector>

#include "detection_engine.hpp"

// Host tests for DetectionEngine. Clause references are to
// docs/superpowers/specs/2026-09-14-disarmed-test-mode-amendment.md.

namespace
{

  using namespace alc;

  constexpr int64_t M_TICK_MS { 100 };
  constexpr int64_t M_START_MS { 10000 };
  constexpr int64_t M_MSEC_PER_SEC { 1000 };
  constexpr uint16_t M_THRESHOLD_LSB { 250 };
  constexpr uint16_t M_COOLDOWN_SECS { 8 };
  constexpr uint16_t M_DELAY_SECS { 30 };
  constexpr uint32_t M_DERIVATION_TICKS { 20 };
  constexpr int M_CONFIGURE_FAILURE { -EIO };
  constexpr int M_STANDBY_FAILURE { -EIO };
  constexpr int M_TIMER_FAILURE { -EIO };
  constexpr int64_t M_RETRY_MS { DetectionEngine::M_COOLDOWN_RETRY_MS };
  constexpr uint32_t M_TICKS_PER_SECOND { DetectionEngine::M_TICKS_PER_SECOND };

  // INT1 toggle periods for "motion during a blanked period" loops.
  constexpr int64_t M_SLOW_TOGGLE_MS { 1000 };
  constexpr int64_t M_FAST_TOGGLE_MS { 200 };
  constexpr bool M_BOTH_STATES[] { false, true };

  // Well past a delay's deadline plus the 5 s delayed-trigger hold, so a trigger
  // that was going to fire has certainly done so.
  constexpr int64_t M_DERIVED_HOLD_MARGIN_MS { 10000 };

  class FakeHardware : public DetectionHardware
  {
    public:
      int ConfigureAccelerometer(uint16_t thresholdLsb, bool& awake) override
      {
        configureCalls++;
        lastThresholdLsb = thresholdLsb;
        if (configureResult < 0) { return configureResult; }
        awake = awakeAfterConfigure;
        return 0;
      }

      int StandbyAccelerometer() override
      {
        standbyCalls++;
        return standbyResult;
      }

      int StartCooldownTimer(uint32_t durationMs) override
      {
        startCooldownCalls++;
        lastCooldownMs = durationMs;
        if (startCooldownResult < 0) { return startCooldownResult; }
        cooldownRunning = true;
        cooldownExpired = false;
        return 0;
      }

      int StopCooldownTimer() override
      {
        stopCooldownCalls++;
        cooldownRunning = false;
        return stopCooldownResult;
      }

      int CooldownTimerExpired(bool& expired) override
      {
        expired = cooldownExpired;
        return 0;
      }

      int ClearCooldownTimerEvent() override
      {
        clearEventCalls++;
        cooldownExpired = false;
        return 0;
      }

      void StartDelayTimer(uint32_t durationMs) override
      {
        startDelayCalls++;
        lastDelayMs = durationMs;
        delayEndMs  = nowMs + static_cast<int64_t>(durationMs);
        delayArmed  = true;
      }

      void StopDelayTimer() override
      {
        stopDelayCalls++;
        delayArmed = false;
      }

      bool DelayTimerRunning() const override { return delayArmed && nowMs < delayEndMs; }

      int SetTriggerPendingScan(bool fast) override
      {
        if (fast) {
          fastScanOnCalls++;
        } else {
          fastScanOffCalls++;
        }
        fastScan = fast;
        return 0;
      }

      bool ScannerRunning() const override { return scannerRunning; }

      void OnDetectionEvent(const DetectionEvent& event) override { events.push_back(event); }

      uint32_t countEvents(DetectionEventType type) const
      {
        uint32_t count { 0 };

        for (const DetectionEvent& event : events) {
          if (event.type == type) { count++; }
        }
        return count;
      }

      int64_t nowMs { M_START_MS };
      int configureResult { 0 };
      bool awakeAfterConfigure { false };
      uint32_t configureCalls { 0 };
      uint16_t lastThresholdLsb { 0 };
      int standbyResult { 0 };
      uint32_t standbyCalls { 0 };
      int startCooldownResult { 0 };
      uint32_t startCooldownCalls { 0 };
      uint32_t lastCooldownMs { 0 };
      uint32_t stopCooldownCalls { 0 };
      int stopCooldownResult { 0 };
      bool cooldownRunning { false };
      bool cooldownExpired { false };
      uint32_t clearEventCalls { 0 };
      uint32_t startDelayCalls { 0 };
      uint32_t stopDelayCalls { 0 };
      uint32_t lastDelayMs { 0 };
      int64_t delayEndMs { 0 };
      bool delayArmed { false };
      uint32_t fastScanOnCalls { 0 };
      uint32_t fastScanOffCalls { 0 };
      bool fastScan { false };
      bool scannerRunning { true };
      std::vector<DetectionEvent> events;
  };

  // One device: the fake, the engine, and App's side of the loop - the output
  // derivation and NoteOutput() - reproduced exactly as Task 2 wires them.
  class Rig
  {
    public:
      Rig(uint8_t activations, uint16_t cooldownSecs, uint16_t delaySecs)
          : engine(hardware)
          , settings { activations, cooldownSecs, delaySecs, M_THRESHOLD_LSB }
      {}

      // The engine holds a reference to this rig's own hardware, so a copy would
      // drive the wrong fake. Returned from makeRig() by guaranteed elision only.
      Rig(const Rig&)            = delete;
      Rig& operator=(const Rig&) = delete;

      int restart(bool armedAfter)
      {
        int result { engine.Restart(settings, armedAfter, hardware.nowMs) };

        // App sets the boolean only on success; a refused arm stays Inactive.
        armed = armedAfter && result == 0;
        return result;
      }

      // One 100 ms loop tick. Returns the output App would derive.
      bool tick(bool awake)
      {
        bool output { false };

        hardware.nowMs += M_TICK_MS;
        engine.Tick(settings, armed, awake, hardware.nowMs);
        output = armed && engine.DetectionMet() && engine.DelayPermitsFiring();
        engine.NoteOutput(output);
        if (output) { everOutput = true; }
        return output;
      }

      // A rising edge, then release.
      void tap()
      {
        tick(true);
        tick(false);
      }

      void idleUntil(int64_t untilMs)
      {
        while (hardware.nowMs < untilMs) {
          tick(false);
        }
      }

      // App's disarm path (App::disarmDevice() -> ArmingSequence::Disarm()),
      // exactly as far as the engine sees it: the fire pins are disabled first
      // (no engine effect, so not modelled), then the state goes Inactive, then
      // App::RestartDetection(false) re-derives the output through
      // updateOutputState(EngineTick::Skip) - no engine tick, so the output is
      // simply false while the device is not Active - then restartEngine(false).
      // No time passes: App does all of it in one call, between two poll-loop
      // ticks.
      void disarm()
      {
        armed = false;
        engine.NoteOutput(false);
        assert(engine.Restart(settings, false, hardware.nowMs) == 0);
      }

      FakeHardware hardware;
      DetectionEngine engine;
      DetectionSettings settings;
      bool armed { false };
      bool everOutput { false };
  };

  // Sub-states a test can be in when it is restarted or armed (amendment 3.2).
  enum class SubState { PartCounted, MidCooldown, MidDelay, DetectionLive, DetectionHold, AwakeAsserted };

  constexpr SubState M_ALL_SUB_STATES[] { SubState::PartCounted,   SubState::MidCooldown,   SubState::MidDelay,
                                          SubState::DetectionLive, SubState::DetectionHold, SubState::AwakeAsserted };

  constexpr uint8_t M_THREE { 3 };
  constexpr uint8_t M_ONE { 1 };

  // Settings for a sub-state. Every case uses N = 3, so the first post-arm edge
  // must read as count 1 - except the hold, which needs a delay.
  Rig makeRig(SubState subState)
  {
    switch (subState) {
      case SubState::MidCooldown:
        return Rig(M_THREE, M_COOLDOWN_SECS, 0);
      case SubState::MidDelay:
      case SubState::DetectionHold:
        return Rig(M_THREE, 0, M_DELAY_SECS);
      default:
        return Rig(M_THREE, 0, 0);
    }
  }

  // Starts a disarmed test and drives it into the sub-state.
  void driveInto(Rig& rig, SubState subState)
  {
    int result { rig.restart(false) };

    assert(result == 0);
    switch (subState) {
      case SubState::PartCounted:
        rig.tap();
        rig.tap();
        assert(rig.engine.ActivationCount() == 2);
        break;
      case SubState::MidCooldown:
        rig.tap();
        assert(rig.engine.InCooldown());
        break;
      case SubState::MidDelay:
        rig.tap();
        rig.tap();
        rig.tap();
        assert(rig.engine.DelayPending() && !rig.engine.DetectionMet());
        break;
      case SubState::DetectionLive:
        rig.tap();
        rig.tap();
        rig.tick(true);
        assert(rig.engine.DetectionMet());
        break;
      case SubState::DetectionHold:
        rig.tap();
        rig.tap();
        rig.tap();
        rig.idleUntil(rig.hardware.delayEndMs);
        assert(rig.engine.DetectionMet() && !rig.engine.DelayPending());
        break;
      case SubState::AwakeAsserted:
        rig.tap();
        rig.tick(true);
        rig.tick(true);
        rig.hardware.awakeAfterConfigure = true;
        break;
    }
  }

  void assertRestartedFromZero(const Rig& rig)
  {
    assert(rig.engine.ActivationCount() == 0);
    assert(!rig.engine.InCooldown());
    assert(!rig.engine.DelayPending());
    assert(!rig.engine.DelayPendingArmed());
    assert(!rig.engine.DetectionMet());
    assert(!rig.hardware.DelayTimerRunning());
  }

  void testCounting()
  {
    // Section 2: activation counting runs identically armed and disarmed.
    for (bool armed : M_BOTH_STATES) {
      Rig rig(M_THREE, 0, 0);
      bool output { false };

      assert(rig.restart(armed) == 0);
      assert(rig.hardware.configureCalls == 1 && rig.hardware.lastThresholdLsb == M_THRESHOLD_LSB);

      // A LEVEL counts once, not once per tick.
      rig.tick(true);
      assert(rig.engine.ActivationCount() == 1);
      for (uint32_t i = 0; i < M_DERIVATION_TICKS; i++) {
        rig.tick(true);
      }
      assert(rig.engine.ActivationCount() == 1);
      rig.tick(false);
      rig.tap();
      assert(rig.engine.ActivationCount() == 2 && !rig.engine.DetectionMet());

      output = rig.tick(true);
      assert(rig.engine.DetectionMet() && rig.engine.ActivationCount() == 0);
      assert(output == armed);
      assert(rig.hardware.countEvents(DetectionEventType::Activation) == 3);
      assert(rig.hardware.events[0].count == 1 && rig.hardware.events[0].limit == M_THREE);

      // Cooldown 0 never stands the part down.
      assert(rig.hardware.startCooldownCalls == 0 && rig.hardware.standbyCalls == 0);

      rig.tick(false);
      assert(!rig.engine.DetectionMet());
    }
    printf("detection engine counting: OK\n");
  }

  void testCooldown()
  {
    // Independent restatement of the fallback: +-10% PMIC tolerance plus 2 s grace.
    constexpr int64_t M_PMIC_TOLERANCE_DIVISOR { 10 };
    constexpr int64_t M_LOOP_GRACE_MS { 2000 };
    constexpr int64_t M_COOLDOWN_MS { M_COOLDOWN_SECS * M_MSEC_PER_SEC };
    constexpr int64_t M_DEADLINE_MS { M_COOLDOWN_MS + M_COOLDOWN_MS / M_PMIC_TOLERANCE_DIVISOR + M_LOOP_GRACE_MS };

    // Section 2: the cooldown between activations runs identically in both states.
    for (bool armed : M_BOTH_STATES) {
      Rig rig(M_THREE, M_COOLDOWN_SECS, 0);
      int64_t cooldownStartMs { 0 };

      assert(rig.restart(armed) == 0);
      rig.tick(true);
      cooldownStartMs = rig.hardware.nowMs;
      assert(rig.engine.ActivationCount() == 1 && rig.engine.InCooldown());
      assert(rig.hardware.standbyCalls == 1 && rig.hardware.startCooldownCalls == 1);
      assert(rig.hardware.lastCooldownMs == M_COOLDOWN_SECS * M_MSEC_PER_SEC);
      assert(rig.hardware.countEvents(DetectionEventType::CooldownStarted) == 1);

      // No counting during the cooldown, whatever INT1 claims.
      for (uint32_t i = 0; i < M_DERIVATION_TICKS; i++) {
        rig.tick(i % 2 == 0);
      }
      assert(rig.engine.ActivationCount() == 1 && rig.engine.InCooldown());

      // PMIC reports expiry: the event is cleared and the part reconfigured.
      rig.hardware.cooldownExpired = true;
      rig.tick(false);
      assert(!rig.engine.InCooldown());
      assert(rig.hardware.clearEventCalls == 1 && rig.hardware.configureCalls == 2);
      assert(rig.hardware.countEvents(DetectionEventType::CooldownElapsed) == 1);
      assert(rig.hardware.countEvents(DetectionEventType::CooldownForced) == 0);

      // Counting resumes; the second activation starts a second cooldown.
      rig.tap();
      assert(rig.engine.ActivationCount() == 2 && rig.engine.InCooldown());

      // Deadline fallback: the PMIC never reports.
      cooldownStartMs = rig.hardware.nowMs - M_TICK_MS;
      while (rig.engine.InCooldown()) {
        assert(rig.hardware.nowMs < cooldownStartMs + M_DEADLINE_MS);
        rig.tick(false);
      }
      assert(rig.hardware.nowMs == cooldownStartMs + M_DEADLINE_MS);
      assert(rig.hardware.countEvents(DetectionEventType::CooldownForced) == 1);

      rig.tick(true);
      assert(rig.engine.DetectionMet());
      assert(!rig.everOutput || armed);
    }

    // N = 1 never starts a cooldown.
    for (bool armed : M_BOTH_STATES) {
      Rig rig(M_ONE, M_COOLDOWN_SECS, 0);

      assert(rig.restart(armed) == 0);
      rig.tick(true);
      assert(rig.engine.DetectionMet() && !rig.engine.InCooldown());
      assert(rig.hardware.startCooldownCalls == 0 && rig.hardware.standbyCalls == 0);
    }
    printf("detection engine cooldown: OK\n");
  }

  void testDelay()
  {
    constexpr int64_t M_HOLD_MS { DetectionEngine::M_DELAYED_TRIGGER_HOLD_MS };

    // Section 2 and owner decisions: the delay runs in both states; fast scan
    // and the PM lock are armed-only.
    for (bool armed : M_BOTH_STATES) {
      Rig rig(M_ONE, 0, M_DELAY_SECS);
      int64_t expiryMs { 0 };

      assert(rig.restart(armed) == 0);
      rig.tick(true);
      assert(rig.engine.DelayPending() && !rig.engine.DetectionMet() && !rig.engine.DelayPermitsFiring());
      assert(rig.hardware.startDelayCalls == 1 && rig.hardware.lastDelayMs == M_DELAY_SECS * M_MSEC_PER_SEC);
      assert(rig.engine.DelayPendingArmed() == armed);
      assert(rig.hardware.fastScanOnCalls == (armed ? 1U : 0U));
      assert(rig.hardware.countEvents(DetectionEventType::DelayStarted) == 1);
      expiryMs = rig.hardware.delayEndMs;

      // Detection waits for the delay; edges during it are not counted.
      while (rig.hardware.nowMs + M_TICK_MS < expiryMs) {
        assert(!rig.tick(rig.hardware.nowMs % M_SLOW_TOGGLE_MS == 0));
        assert(!rig.engine.DetectionMet());
      }
      assert(rig.hardware.countEvents(DetectionEventType::Activation) == 1);

      assert(rig.tick(false) == armed);
      assert(rig.hardware.nowMs == expiryMs);
      assert(rig.engine.DetectionMet() && !rig.engine.DelayPending());
      assert(rig.hardware.countEvents(DetectionEventType::DelayExpired) == 1);
      assert(!rig.hardware.fastScan);
      assert(rig.hardware.fastScanOnCalls == (armed ? 1U : 0U));

      // The hold keeps detection for M_DELAYED_TRIGGER_HOLD_MS with AWAKE clear.
      while (rig.hardware.nowMs + M_TICK_MS < expiryMs + M_HOLD_MS) {
        rig.tick(false);
        assert(rig.engine.DetectionMet());
      }
      rig.tick(false);
      assert(!rig.engine.DetectionMet());
    }
    printf("detection engine delay: OK\n");
  }

  void testRestartFromEverySubState()
  {
    // Section 3: Disarm and Settings restart the test from zero, from anywhere.
    for (SubState subState : M_ALL_SUB_STATES) {
      Rig rig { makeRig(subState) };
      uint32_t configureBefore { 0 };
      uint32_t stopCooldownBefore { 0 };
      uint32_t stopDelayBefore { 0 };
      bool wasInCooldown { false };

      driveInto(rig, subState);
      if (subState == SubState::AwakeAsserted) {
        assert(rig.restart(false) == 0);
        assert(rig.engine.IgnoringStaleAwake());
        rig.hardware.awakeAfterConfigure = false;
      }

      configureBefore    = rig.hardware.configureCalls;
      stopCooldownBefore = rig.hardware.stopCooldownCalls;
      stopDelayBefore    = rig.hardware.stopDelayCalls;
      wasInCooldown      = rig.engine.InCooldown();

      assert(rig.restart(false) == 0);
      assertRestartedFromZero(rig);
      assert(!rig.engine.IgnoringStaleAwake());
      assert(!rig.engine.TakeTriggerComplete());
      assert(rig.hardware.configureCalls == configureBefore + 1);
      assert(rig.hardware.stopDelayCalls == stopDelayBefore + 1);
      assert(rig.hardware.stopCooldownCalls == stopCooldownBefore + (wasInCooldown ? 1U : 0U));

      // And counting starts again at 1.
      rig.tap();
      assert(rig.engine.ActivationCount() == 1);
    }
    printf("detection engine restart from zero: OK\n");
  }

  void testArmingMidTest()
  {
    constexpr uint32_t M_COOLDOWNS_TO_OUTLAST { 2 };
    constexpr uint32_t M_PAST_ANY_DEADLINE_TICKS { (M_DELAY_SECS + M_COOLDOWN_SECS * M_COOLDOWNS_TO_OUTLAST) * M_TICKS_PER_SECOND };

    // ================================================================
    //  SAFETY - section 3.2. Arming at any moment of a test starts the
    //  armed session from zero, and NO state from the test can reach
    //  the fire output.
    // ================================================================
    for (SubState subState : M_ALL_SUB_STATES) {
      Rig rig { makeRig(subState) };

      driveInto(rig, subState);
      assert(!rig.everOutput);

      // Arm.
      assert(rig.restart(true) == 0);
      assert(rig.armed);
      assertRestartedFromZero(rig);
      assert(!rig.engine.TakeTriggerComplete());
      assert(rig.hardware.fastScanOnCalls == 0);

      if (subState == SubState::AwakeAsserted) {
        // AWAKE asserted at arming is ignored while it stays asserted...
        assert(rig.engine.IgnoringStaleAwake());
        rig.hardware.awakeAfterConfigure = false;
        for (uint32_t i = 0; i < M_DERIVATION_TICKS; i++) {
          assert(!rig.tick(true));
        }
        assert(rig.engine.ActivationCount() == 0 && !rig.engine.DetectionMet());

        // ...until it drops and a fresh edge arrives.
        rig.tick(false);
        assert(!rig.engine.IgnoringStaleAwake());
      }

      // A single new edge is activation 1, not detection.
      assert(!rig.tick(true));
      assert(rig.engine.ActivationCount() == 1 && !rig.engine.DetectionMet() && !rig.engine.DelayPending());
      for (uint32_t i = 0; i < M_DERIVATION_TICKS; i++) {
        assert(!rig.tick(false));
      }

      // Past the original test delay's deadline, the hold and any cooldown: the
      // old test delay never fires the armed device.
      for (uint32_t i = 0; i < M_PAST_ANY_DEADLINE_TICKS; i++) {
        assert(!rig.tick(false));
        assert(!rig.engine.DetectionMet());
      }
      assert(!rig.everOutput);
      assert(rig.hardware.fastScanOnCalls == 0);
    }
    printf("detection engine SAFETY arming mid-test: OK\n");
  }

  void testConfigureFailureOnArm()
  {
    constexpr uint32_t M_SECOND_RETRY { 2 };

    // Section 3: a failed configure refuses the arm, and the engine retries at 1 Hz.
    Rig rig(M_THREE, 0, 0);
    int64_t failedAtMs { 0 };
    int result { 0 };

    assert(rig.restart(false) == 0);
    rig.tap();

    rig.hardware.configureResult = M_CONFIGURE_FAILURE;
    result                       = rig.restart(true);
    failedAtMs                   = rig.hardware.nowMs;
    assert(result == M_CONFIGURE_FAILURE && !rig.armed);
    assert(rig.hardware.configureCalls == 2);
    assert(rig.hardware.standbyCalls == 1);

    // Everything from the test is discarded; InCooldown() now reports the pending retry.
    assert(rig.engine.ActivationCount() == 0 && !rig.engine.DetectionMet());
    assert(!rig.engine.DelayPending() && rig.engine.InCooldown());

    // No more than one attempt per M_COOLDOWN_RETRY_MS, and no counting meanwhile.
    while (rig.hardware.nowMs + M_TICK_MS < failedAtMs + M_RETRY_MS) {
      assert(!rig.tick(rig.hardware.nowMs % M_FAST_TOGGLE_MS == 0));
      assert(rig.hardware.configureCalls == 2);
    }
    rig.tick(false);
    assert(rig.hardware.configureCalls == 3);
    while (rig.hardware.nowMs + M_TICK_MS < failedAtMs + M_SECOND_RETRY * M_RETRY_MS) {
      rig.tick(false);
      assert(rig.hardware.configureCalls == 3);
    }
    assert(rig.engine.ActivationCount() == 0);

    // The part responds again: the next retry succeeds and detection resumes.
    rig.hardware.configureResult = 0;
    rig.tick(false);
    assert(rig.hardware.configureCalls == 4 && !rig.engine.InCooldown());
    assert(rig.hardware.countEvents(DetectionEventType::CooldownElapsed) == 1);
    rig.tap();
    assert(rig.engine.ActivationCount() == 1);
    assert(!rig.everOutput);
    assert(rig.hardware.countEvents(DetectionEventType::RestartStandbyFailed) == 0);

    // A refused arm is not tagged armed: an output note after it is no one-shot.
    {
      Rig refused(M_ONE, 0, 0);

      refused.hardware.configureResult = M_CONFIGURE_FAILURE;
      assert(refused.restart(true) == M_CONFIGURE_FAILURE);
      refused.engine.NoteOutput(true);
      refused.engine.NoteOutput(false);
      assert(!refused.engine.TakeTriggerComplete());
    }

    // The best-effort standby after a refused arm fails too: reported, and the
    // retry still runs.
    {
      Rig refused(M_THREE, 0, 0);

      refused.hardware.configureResult = M_CONFIGURE_FAILURE;
      refused.hardware.standbyResult   = M_STANDBY_FAILURE;
      assert(refused.restart(true) == M_CONFIGURE_FAILURE);
      assert(refused.hardware.countEvents(DetectionEventType::RestartStandbyFailed) == 1);
      assert(refused.hardware.events.back().armed && refused.hardware.events.back().result == M_STANDBY_FAILURE);
      refused.hardware.configureResult = 0;
      refused.idleUntil(refused.hardware.nowMs + M_RETRY_MS);
      assert(!refused.engine.InCooldown());
      refused.tap();
      assert(refused.engine.ActivationCount() == 1);
    }

    // A Restart that cannot stop a running cooldown timer reports it and still
    // discards the cooldown.
    {
      Rig stuck(M_THREE, M_COOLDOWN_SECS, 0);

      assert(stuck.restart(false) == 0);
      stuck.tap();
      assert(stuck.engine.InCooldown());
      stuck.hardware.stopCooldownResult = M_TIMER_FAILURE;
      assert(stuck.restart(true) == 0);
      assert(stuck.hardware.countEvents(DetectionEventType::RestartCooldownStopFailed) == 1);
      assert(!stuck.engine.InCooldown() && stuck.engine.ActivationCount() == 0);
    }
    printf("detection engine configure failure on arm: OK\n");
  }

  void testOneShot()
  {
    constexpr int64_t M_HOLD_MS { DetectionEngine::M_DELAYED_TRIGGER_HOLD_MS };

    // Armed one-shot, driven directly: asserted then ended completes once.
    {
      Rig rig(M_ONE, 0, 0);

      assert(rig.restart(true) == 0);
      rig.engine.NoteOutput(true);
      assert(!rig.engine.TakeTriggerComplete());
      rig.engine.NoteOutput(false);
      assert(rig.engine.TakeTriggerComplete());
      assert(!rig.engine.TakeTriggerComplete());
    }

    // Section 3: armed completion latches (App restarts); disarmed carries on.
    for (bool armed : M_BOTH_STATES) {
      Rig rig(M_ONE, 0, M_DELAY_SECS);
      uint32_t activations { 0 };

      assert(rig.restart(armed) == 0);
      rig.tap();
      rig.idleUntil(rig.hardware.delayEndMs);
      assert(rig.engine.DetectionMet());
      activations = rig.hardware.countEvents(DetectionEventType::Activation);

      // No counting during the detection period, armed or not.
      while (rig.engine.DetectionMet()) {
        rig.tick(rig.hardware.nowMs % M_FAST_TOGGLE_MS == 0);
        if (rig.engine.DetectionMet()) { assert(!rig.engine.TakeTriggerComplete()); }
      }
      assert(rig.hardware.countEvents(DetectionEventType::Activation) == activations);
      assert(rig.hardware.nowMs >= rig.hardware.delayEndMs + M_HOLD_MS);

      assert(rig.engine.TakeTriggerComplete() == armed);
      assert(!rig.engine.TakeTriggerComplete());

      if (!armed) {
        // Counting resumes: the next tap starts a new test delay.
        rig.tick(false);
        rig.tap();
        assert(rig.hardware.countEvents(DetectionEventType::Activation) == activations + 1);
        assert(rig.engine.DelayPending());
      }
    }
    printf("detection engine one-shot: OK\n");
  }

  void testWatchdog()
  {
    constexpr uint32_t M_STUCK_TICKS { DetectionEngine::M_AWAKE_STUCK_TICKS };

    // Section 3.3: the stuck-AWAKE watchdog runs in both states.
    for (bool armed : M_BOTH_STATES) {
      Rig rig(M_ONE, 0, 0);
      uint32_t configureBefore { 0 };

      assert(rig.restart(armed) == 0);
      configureBefore = rig.hardware.configureCalls;

      for (uint32_t i = 0; i + 1 < M_STUCK_TICKS; i++) {
        rig.tick(true);
      }
      assert(rig.engine.DetectionMet() && rig.hardware.configureCalls == configureBefore);

      // The part still reports AWAKE after the re-arm: suppression is kept.
      rig.hardware.awakeAfterConfigure = true;
      rig.tick(true);
      assert(rig.hardware.configureCalls == configureBefore + 1);
      assert(!rig.engine.DetectionMet());
      assert(rig.engine.IgnoringStaleAwake());
      assert(rig.hardware.countEvents(DetectionEventType::WatchdogRearm) == 1);
      for (const DetectionEvent& event : rig.hardware.events) {
        if (event.type == DetectionEventType::WatchdogRearm) { assert(event.seconds == M_STUCK_TICKS / M_TICKS_PER_SECOND); }
      }
      rig.tick(true);
      assert(!rig.engine.DetectionMet() && rig.engine.ActivationCount() == 0);
    }

    // A failed re-arm hands over to the 1 Hz retry path.
    for (bool armed : M_BOTH_STATES) {
      Rig rig(M_THREE, 0, 0);
      uint32_t configureBefore { 0 };

      assert(rig.restart(armed) == 0);
      configureBefore              = rig.hardware.configureCalls;
      rig.hardware.configureResult = M_CONFIGURE_FAILURE;
      for (uint32_t i = 0; i < M_STUCK_TICKS; i++) {
        rig.tick(true);
      }
      assert(rig.hardware.configureCalls == configureBefore + 1);
      assert(rig.hardware.countEvents(DetectionEventType::WatchdogRearmFailed) == 1);
      assert(rig.engine.InCooldown());

      // Retried on the very next tick, then at 1 Hz.
      rig.tick(false);
      assert(rig.hardware.configureCalls == configureBefore + 2);
      rig.hardware.configureResult = 0;
      rig.tick(false);
      assert(rig.hardware.configureCalls == configureBefore + 2);
      rig.idleUntil(rig.hardware.nowMs + M_RETRY_MS);
      assert(rig.hardware.configureCalls == configureBefore + 3 && !rig.engine.InCooldown());
    }
    printf("detection engine watchdog: OK\n");
  }

  void testScanLostDuringArmedDelay()
  {
    // Owner rule 2026-09-14: always fail safe. App disarms as soon as the scanner
    // is down while Active; the engine never fires an armed delay that ran
    // without a scanner, even if that disarm did not happen.
    {
      Rig rig(M_ONE, 0, M_DELAY_SECS);
      int64_t deadlineMs { 0 };

      assert(rig.restart(true) == 0);
      rig.tap();
      assert(rig.engine.DelayPendingArmed());
      assert(rig.hardware.countEvents(DetectionEventType::DelayScanLost) == 0);
      deadlineMs = rig.hardware.delayEndMs;

      // The scanner drops out for a while, then recovers - the loss is remembered.
      rig.hardware.scannerRunning = false;
      rig.tick(false);
      rig.tick(false);
      rig.hardware.scannerRunning = true;

      rig.idleUntil(deadlineMs + M_DERIVED_HOLD_MARGIN_MS);
      assert(!rig.engine.DelayPendingArmed());
      assert(!rig.engine.DetectionMet() && !rig.everOutput);
      assert(!rig.engine.TakeTriggerComplete());
      assert(rig.hardware.countEvents(DetectionEventType::DelayExpired) == 0);
      assert(rig.hardware.countEvents(DetectionEventType::DelayExpiredScanLostSuppressed) == 1);
      assert(!rig.hardware.fastScan);
    }

    // Down at the start of the delay: reported at once, suppressed at expiry.
    {
      Rig rig(M_ONE, 0, M_DELAY_SECS);
      int64_t deadlineMs { 0 };

      assert(rig.restart(true) == 0);
      rig.hardware.scannerRunning = false;
      rig.tap();
      assert(rig.hardware.countEvents(DetectionEventType::DelayScanLost) == 1);
      rig.hardware.scannerRunning = true;
      deadlineMs                  = rig.hardware.delayEndMs;
      rig.idleUntil(deadlineMs + M_DERIVED_HOLD_MARGIN_MS);
      assert(!rig.engine.DetectionMet() && !rig.everOutput);
      assert(rig.hardware.countEvents(DetectionEventType::DelayExpired) == 0);
      assert(rig.hardware.countEvents(DetectionEventType::DelayExpiredScanLostSuppressed) == 1);
    }

    // A suppressed delay leaves the engine counting: the next genuine delay, with
    // a healthy scanner, fires as normal.
    {
      Rig rig(M_ONE, 0, M_DELAY_SECS);

      assert(rig.restart(true) == 0);
      rig.tap();
      rig.hardware.scannerRunning = false;
      rig.tick(false);
      rig.hardware.scannerRunning = true;
      rig.idleUntil(rig.hardware.delayEndMs + M_DERIVED_HOLD_MARGIN_MS);
      assert(!rig.everOutput);

      rig.tap();
      assert(rig.engine.DelayPendingArmed());
      rig.idleUntil(rig.hardware.delayEndMs);
      while (!rig.engine.DetectionMet()) {
        rig.tick(false);
      }
      assert(rig.everOutput);
      assert(rig.hardware.countEvents(DetectionEventType::DelayExpiredScanLostSuppressed) == 1);
    }

    // A disarmed test delay is unaffected by the scanner: it never fires the pins,
    // so it still shows detection even with the scanner down throughout.
    {
      Rig rig(M_ONE, 0, M_DELAY_SECS);

      assert(rig.restart(false) == 0);
      rig.hardware.scannerRunning = false;
      rig.tap();
      rig.tick(false);
      rig.idleUntil(rig.hardware.delayEndMs);
      while (rig.engine.DelayPendingArmed() || !rig.engine.DetectionMet()) {
        rig.tick(false);
      }
      assert(rig.engine.DetectionMet() && !rig.everOutput);
      assert(rig.hardware.countEvents(DetectionEventType::DelayExpired) == 1);
      assert(rig.hardware.countEvents(DetectionEventType::DelayScanLost) == 0);
      assert(rig.hardware.countEvents(DetectionEventType::DelayExpiredScanLostSuppressed) == 0);
    }

    // Healthy scanner, armed: fires, no scan-lost report.
    {
      Rig rig(M_ONE, 0, M_DELAY_SECS);

      assert(rig.restart(true) == 0);
      rig.tap();
      rig.idleUntil(rig.hardware.delayEndMs);
      while (!rig.engine.DetectionMet()) {
        rig.tick(false);
      }
      assert(rig.everOutput);
      assert(rig.hardware.countEvents(DetectionEventType::DelayScanLost) == 0);
      assert(rig.hardware.countEvents(DetectionEventType::DelayExpiredScanLostSuppressed) == 0);
    }
    printf("detection engine scan lost during armed delay: OK\n");
  }

  // Ticks until the next configure attempt, asserting it comes no sooner than
  // M_COOLDOWN_RETRY_MS after the previous one and no later than one tick past.
  void expectRetryAfter(Rig& rig, int64_t previousAttemptMs)
  {
    uint32_t configureBefore { rig.hardware.configureCalls };

    while (rig.hardware.nowMs + M_TICK_MS < previousAttemptMs + M_RETRY_MS) {
      assert(!rig.tick(rig.hardware.nowMs % M_FAST_TOGGLE_MS == 0));
      assert(rig.hardware.configureCalls == configureBefore);
    }
    rig.tick(false);
    assert(rig.hardware.configureCalls == configureBefore + 1);
  }

  void testCooldownFailures()
  {
    // Every cooldown failure branch must end with detection restored or a 1 Hz
    // retry pending - never a part stood down with nothing to bring it back.
    for (bool armed : M_BOTH_STATES) {
      // Timer start fails AND the restoring configure fails.
      {
        Rig rig(M_THREE, M_COOLDOWN_SECS, 0);
        int64_t attemptMs { 0 };

        assert(rig.restart(armed) == 0);
        rig.hardware.startCooldownResult = M_TIMER_FAILURE;
        rig.hardware.stopCooldownResult  = M_TIMER_FAILURE;
        rig.hardware.configureResult     = M_CONFIGURE_FAILURE;
        rig.tick(true);
        attemptMs = rig.hardware.nowMs;
        assert(rig.hardware.countEvents(DetectionEventType::CooldownTimerFailed) == 1);
        assert(rig.hardware.countEvents(DetectionEventType::CooldownTimerStopFailed) == 1);
        assert(rig.hardware.countEvents(DetectionEventType::CooldownRestoreFailed) == 1);
        assert(rig.engine.InCooldown() && rig.engine.ActivationCount() == 1);

        // A configure attempt every M_COOLDOWN_RETRY_MS, no counting meanwhile.
        expectRetryAfter(rig, attemptMs);
        attemptMs = rig.hardware.nowMs;
        expectRetryAfter(rig, attemptMs);
        attemptMs = rig.hardware.nowMs;
        assert(rig.engine.InCooldown() && rig.engine.ActivationCount() == 1);

        // The PMIC timer that never started is never consulted for expiry.
        rig.hardware.cooldownExpired = true;
        rig.tick(false);
        assert(rig.hardware.clearEventCalls == 0);

        rig.hardware.configureResult     = 0;
        rig.hardware.startCooldownResult = 0;
        expectRetryAfter(rig, attemptMs);
        assert(!rig.engine.InCooldown());
        rig.tap();
        assert(rig.engine.ActivationCount() == 2);
        assert(!rig.everOutput);
      }

      // Timer start fails, restore succeeds: fail toward detecting at once.
      {
        Rig rig(M_THREE, M_COOLDOWN_SECS, 0);

        assert(rig.restart(armed) == 0);
        rig.hardware.startCooldownResult = M_TIMER_FAILURE;
        rig.tick(true);
        assert(rig.hardware.countEvents(DetectionEventType::CooldownTimerFailed) == 1);
        assert(rig.hardware.countEvents(DetectionEventType::CooldownRestoreFailed) == 0);
        assert(!rig.engine.InCooldown());
        rig.tick(false);
        rig.tick(true);
        assert(rig.engine.ActivationCount() == 2);
      }

      // Standby fails part-way (INT1 may already be unmapped).
      {
        Rig rig(M_THREE, M_COOLDOWN_SECS, 0);
        uint32_t configureBefore { 0 };
        int64_t attemptMs { 0 };

        assert(rig.restart(armed) == 0);
        configureBefore            = rig.hardware.configureCalls;
        rig.hardware.standbyResult = M_STANDBY_FAILURE;
        rig.tick(true);
        attemptMs = rig.hardware.nowMs;
        assert(rig.hardware.countEvents(DetectionEventType::CooldownStandbyFailed) == 1);
        assert(rig.hardware.startCooldownCalls == 0);
        assert(rig.engine.InCooldown());

        // The full bootstrap runs within M_COOLDOWN_RETRY_MS.
        rig.hardware.standbyResult = 0;
        expectRetryAfter(rig, attemptMs);
        assert(rig.hardware.configureCalls == configureBefore + 1 && !rig.engine.InCooldown());
        rig.tap();
        assert(rig.engine.ActivationCount() == 2);
        assert(!rig.everOutput);
      }
    }
    printf("detection engine cooldown failures: OK\n");
  }

  // The session after an accepted arm is fresh: nothing pending retries, nothing
  // counted, no output for the derivation window and past every deadline, and
  // the first edge reads as activation 1 (or, for N = 1, is the first fire).
  void assertFreshArmedSession(Rig& rig, uint32_t configureAtArm)
  {
    constexpr uint32_t M_PAST_RETRY_AND_DEADLINES_TICKS { (M_DELAY_SECS + M_COOLDOWN_SECS) * M_TICKS_PER_SECOND };

    assert(rig.armed);
    assertRestartedFromZero(rig);
    assert(!rig.engine.IgnoringStaleAwake());
    for (uint32_t i = 0; i < M_PAST_RETRY_AND_DEADLINES_TICKS; i++) {
      assert(!rig.tick(false));
    }

    // No stale retry configured the part again, and nothing was counted.
    assert(rig.hardware.configureCalls == configureAtArm);
    assert(rig.engine.ActivationCount() == 0 && !rig.everOutput);

    if (rig.settings.activations > 1) {
      assert(!rig.tick(true));
      assert(rig.engine.ActivationCount() == 1);
    } else {
      // Teeth: the rig can fire, so the silence above was the engine's doing.
      assert(rig.tick(true));
    }
  }

  void testArmingFromRetryState()
  {
    constexpr uint8_t M_ACTIVATION_CASES[] { M_ONE, M_THREE };

    // SAFETY - section 3.2 applied to the retry path.
    for (uint8_t activations : M_ACTIVATION_CASES) {
      // (a) Disarmed watchdog re-arm fails -> retry pending -> Arm succeeds.
      {
        Rig rig(activations, 0, 0);

        assert(rig.restart(false) == 0);
        rig.hardware.configureResult = M_CONFIGURE_FAILURE;
        for (uint32_t i = 0; i < DetectionEngine::M_AWAKE_STUCK_TICKS; i++) {
          rig.tick(true);
        }
        assert(rig.engine.InCooldown());
        rig.hardware.configureResult = 0;
        assert(rig.restart(true) == 0);
        assertFreshArmedSession(rig, rig.hardware.configureCalls);
      }

      // (b) Refused arm -> accepted arm.
      {
        Rig rig(activations, 0, 0);

        assert(rig.restart(false) == 0);
        rig.tick(true);
        rig.hardware.configureResult = M_CONFIGURE_FAILURE;
        assert(rig.restart(true) == M_CONFIGURE_FAILURE && !rig.armed);
        assert(!rig.engine.DetectionMet() && rig.engine.ActivationCount() == 0);
        for (uint32_t i = 0; i < M_DERIVATION_TICKS; i++) {
          assert(!rig.tick(i % 2 == 0));
        }
        rig.hardware.configureResult = 0;
        assert(rig.restart(true) == 0);
        assertFreshArmedSession(rig, rig.hardware.configureCalls);
      }
    }
    printf("detection engine SAFETY arming from the retry state: OK\n");
  }

  void testArmingMidTestSingleActivation()
  {
    // SAFETY - section 3.2 with N = 1, delay 0: the case where ANY inherited edge,
    // level or latch would fire the output on the spot.

    // A test detection in progress, AWAKE still asserted and still reported by
    // STATUS after the configure.
    {
      Rig rig(M_ONE, 0, 0);

      assert(rig.restart(false) == 0);
      rig.tick(true);
      rig.tick(true);
      assert(rig.engine.DetectionMet() && !rig.everOutput);

      rig.hardware.awakeAfterConfigure = true;
      assert(rig.restart(true) == 0);
      rig.hardware.awakeAfterConfigure = false;
      for (uint32_t i = 0; i < M_DERIVATION_TICKS; i++) {
        assert(!rig.tick(true));
      }
      for (uint32_t i = 0; i < M_DERIVATION_TICKS; i++) {
        assert(!rig.tick(false));
      }
      assert(!rig.everOutput);

      // A fresh edge after release does fire - so the silence above has teeth.
      assert(rig.tick(true));
    }

    // A test detection in progress, released at arming.
    {
      Rig rig(M_ONE, 0, 0);

      assert(rig.restart(false) == 0);
      rig.tick(true);
      assert(rig.engine.DetectionMet());
      assert(rig.restart(true) == 0);
      for (uint32_t i = 0; i < M_DERIVATION_TICKS; i++) {
        assert(!rig.tick(false));
      }
      assert(!rig.everOutput);
      assert(rig.tick(true));
    }

    // A test delay pending at arming (N = 1, delay 30 s) never fires.
    {
      Rig rig(M_ONE, 0, M_DELAY_SECS);
      int64_t oldDeadlineMs { 0 };

      assert(rig.restart(false) == 0);
      rig.tap();
      oldDeadlineMs = rig.hardware.delayEndMs;
      assert(rig.restart(true) == 0);
      rig.idleUntil(oldDeadlineMs + DetectionEngine::M_DELAYED_TRIGGER_HOLD_MS);
      assert(!rig.everOutput && !rig.engine.DetectionMet());
    }
    printf("detection engine SAFETY arming mid-test, N = 1: OK\n");
  }

  void testAppDisarmSeam()
  {
    // App's seam, reproduced exactly (Rig::disarm() mirrors
    // App::disarmDevice(); the Run() latch is reproduced inline below,
    // mirroring App::Run()'s TakeTriggerComplete() check after
    // updateOutputState()). Both cases are safety-critical: a stale
    // m_trigger_complete surviving a restart would report a trigger the
    // moment the device is next armed, with no activation behind it.

    // (a) Output asserted while armed, then App's disarm cuts it off mid-
    // assertion (fire pins disabled first, then Inactive, output re-derived
    // false, NoteOutput(false) still runs against the engine's own m_armed -
    // still true at that point - before Restart() clears it).
    // TakeTriggerComplete() must read false afterwards, and a fresh arm must
    // show no stale completion and no output until a fresh, complete count.
    {
      Rig rig(M_ONE, 0, 0);

      assert(rig.restart(true) == 0);
      assert(rig.tick(true)); // N = 1, no delay: output asserts on the first edge.
      rig.disarm();           // Disarmed mid-assertion - not a natural completion.
      assert(!rig.engine.TakeTriggerComplete());

      assert(rig.restart(true) == 0);
      assert(!rig.engine.TakeTriggerComplete());
      for (uint32_t i = 0; i < M_DERIVATION_TICKS; i++) {
        assert(!rig.tick(false)); // no output until a fresh count completes.
      }
      assert(rig.tick(true));                    // a fresh, complete count still fires - the silence above has teeth.
      assert(!rig.engine.TakeTriggerComplete()); // still asserted, not yet ended.
    }

    // (b) An armed trigger completes naturally, Run()'s latch disarms exactly
    // as App::Run() does, and the restarted test resumes counting at
    // Activation 1 with no second completion reported and no output - the
    // session is disarmed, so NoteOutput() is a no-op regardless.
    {
      Rig rig(M_THREE, 0, 0);

      assert(rig.restart(true) == 0);
      rig.tap();
      rig.tap();
      assert(rig.engine.ActivationCount() == 2);

      assert(rig.tick(true));   // third edge: detection met, output ON.
      assert(!rig.tick(false)); // AWAKE clears: output ends, completion now pending.

      // App::Run()'s latch: TakeTriggerComplete() && armed -> disarm.
      assert(rig.engine.TakeTriggerComplete() && rig.armed);
      rig.disarm();
      assert(!rig.engine.TakeTriggerComplete());

      rig.tap();
      assert(rig.engine.ActivationCount() == 1);
      assert(!rig.engine.TakeTriggerComplete());

      rig.tap();
      assert(rig.engine.ActivationCount() == 2);
      assert(!rig.tick(true)); // third edge: detection met, but disarmed - no output.
      assert(rig.engine.DetectionMet());
      assert(!rig.engine.TakeTriggerComplete()); // disarmed: NoteOutput() is a no-op, so no second completion.
      rig.tick(false);
      assert(!rig.engine.DetectionMet());
      assert(!rig.engine.TakeTriggerComplete());
    }
    printf("detection engine App disarm and Run() latch seam: OK\n");
  }

}

void run_detection_engine_tests()
{
  testCounting();
  testCooldown();
  testDelay();
  testRestartFromEverySubState();
  testArmingMidTest();
  testArmingMidTestSingleActivation();
  testArmingFromRetryState();
  testCooldownFailures();
  testConfigureFailureOnArm();
  testOneShot();
  testWatchdog();
  testScanLostDuringArmedDelay();
  testAppDisarmSeam();
}
