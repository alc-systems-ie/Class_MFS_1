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
  constexpr bool M_BOTH_STATES[] { false, true };

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
        return 0;
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
    constexpr int64_t M_DEADLINE_MS { M_COOLDOWN_SECS * M_MSEC_PER_SEC + (M_COOLDOWN_SECS * M_MSEC_PER_SEC) / 10 + 2000 };

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
        assert(!rig.tick(rig.hardware.nowMs % 1000 == 0));
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
    constexpr uint32_t M_PAST_ANY_DEADLINE_TICKS { (M_DELAY_SECS + M_COOLDOWN_SECS * 2) * 10 };

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
    constexpr int64_t M_RETRY_MS { DetectionEngine::M_COOLDOWN_RETRY_MS };

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
      assert(!rig.tick(rig.hardware.nowMs % 200 == 0));
      assert(rig.hardware.configureCalls == 2);
    }
    rig.tick(false);
    assert(rig.hardware.configureCalls == 3);
    while (rig.hardware.nowMs + M_TICK_MS < failedAtMs + 2 * M_RETRY_MS) {
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
        rig.tick(rig.hardware.nowMs % 200 == 0);
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
        if (event.type == DetectionEventType::WatchdogRearm) { assert(event.seconds == M_STUCK_TICKS / 10); }
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
      rig.idleUntil(rig.hardware.nowMs + DetectionEngine::M_COOLDOWN_RETRY_MS);
      assert(rig.hardware.configureCalls == configureBefore + 3 && !rig.engine.InCooldown());
    }
    printf("detection engine watchdog: OK\n");
  }

  void testScanLostDuringArmedDelay()
  {
    // Preserved: the alarm is prioritised over a possibly missed disarm.
    {
      Rig rig(M_ONE, 0, M_DELAY_SECS);

      assert(rig.restart(true) == 0);
      rig.tap();
      assert(rig.engine.DelayPendingArmed());
      assert(rig.hardware.countEvents(DetectionEventType::DelayScanLost) == 0);

      // The scanner drops out for a while, then recovers - the loss is remembered.
      rig.hardware.scannerRunning = false;
      rig.tick(false);
      rig.tick(false);
      rig.hardware.scannerRunning = true;

      while (!rig.engine.DetectionMet()) {
        rig.tick(false);
      }
      assert(rig.everOutput);
      assert(rig.hardware.countEvents(DetectionEventType::DelayExpired) == 1);
      assert(rig.hardware.countEvents(DetectionEventType::DelayExpiredScanLost) == 1);
    }

    // Down at the start of the delay: reported at once, and again at expiry.
    {
      Rig rig(M_ONE, 0, M_DELAY_SECS);

      assert(rig.restart(true) == 0);
      rig.hardware.scannerRunning = false;
      rig.tap();
      assert(rig.hardware.countEvents(DetectionEventType::DelayScanLost) == 1);
      rig.hardware.scannerRunning = true;
      rig.idleUntil(rig.hardware.delayEndMs);
      assert(rig.engine.DetectionMet() && rig.everOutput);
      assert(rig.hardware.countEvents(DetectionEventType::DelayExpiredScanLost) == 1);
    }

    // Healthy scanner, or a disarmed test delay: no scan-lost report.
    for (bool armed : M_BOTH_STATES) {
      Rig rig(M_ONE, 0, M_DELAY_SECS);

      assert(rig.restart(armed) == 0);
      if (!armed) { rig.hardware.scannerRunning = false; }
      rig.tap();
      rig.idleUntil(rig.hardware.delayEndMs);
      assert(rig.engine.DetectionMet());
      assert(rig.hardware.countEvents(DetectionEventType::DelayScanLost) == 0);
      assert(rig.hardware.countEvents(DetectionEventType::DelayExpiredScanLost) == 0);
    }
    printf("detection engine scan lost during armed delay: OK\n");
  }

}

void run_detection_engine_tests()
{
  testCounting();
  testCooldown();
  testDelay();
  testRestartFromEverySubState();
  testArmingMidTest();
  testConfigureFailureOnArm();
  testOneShot();
  testWatchdog();
  testScanLostDuringArmedDelay();
}
