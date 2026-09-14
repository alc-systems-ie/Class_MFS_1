#include <cassert>
#include <cerrno>
#include <cstdio>
#include <vector>

#include "arming_sequence.hpp"

// Host tests for ArmingSequence. Clause references are to
// docs/superpowers/specs/2026-09-14-arming-sequence-amendment.md.

namespace
{

  using namespace alc;

  constexpr int64_t M_START_MS { 50000 };
  constexpr int64_t M_ARMING_DELAY_MS { ArmingSequence::M_EXIT_DELAY_MS };
  constexpr int64_t M_ONE_MS { 1 };
  constexpr int64_t M_MID_DELAY_MS { 5000 };
  constexpr int64_t M_WELL_PAST_MS { 60000 };
  constexpr int M_RESTART_FAILURE { -EIO };
  constexpr int M_ENABLE_FAILURE { -ENODEV };
  constexpr int M_DISABLE_FAILURE { -EBUSY };
  constexpr int M_SCANNER_FAILURE { -ENODEV };
  constexpr bool M_BOTH_CALLBACKS[] { false, true };

  enum class Call : uint8_t { DisablePins, RestartArmed, RestartDisarmed, EnablePins, Warning };

  struct LogEntry
  {
      Call call;
      ArmState stateAtCall;
      ArmingStep step;
      int result;
  };

  // Records every action in order, with the sequence state observed at the
  // moment of the call, and returns configurable results.
  class FakeActions : public ArmingActions
  {
    public:
      int DisableFirePins() override
      {
        record(Call::DisablePins);
        // Set by failure-path tests, where the sequence must never have been Active.
        if (forbidActiveOnDisable && sequence != nullptr) { assert(sequence->State() != ArmState::Active); }
        return disableResult;
      }

      int RestartDetection(bool armed) override
      {
        record(armed ? Call::RestartArmed : Call::RestartDisarmed);
        // Section 3: the engine's armed session never runs while the state is Active.
        if (sequence != nullptr) { assert(sequence->State() != ArmState::Active); }
        if (armed && disarmInsideRestartArmed) {
          disarmInsideRestartArmed = false;
          nestedDisarm();
        }
        return armed ? restartArmedResult : 0;
      }

      int EnableFirePins() override
      {
        record(Call::EnablePins);
        // Section 3 step 2.3: Active is set only AFTER the enable returned 0.
        if (sequence != nullptr) { assert(sequence->State() != ArmState::Active); }
        if (disarmInsideEnable) {
          disarmInsideEnable = false;
          nestedDisarm();
        }
        return enableResult;
      }

      bool ScannerRunning() const override
      {
        scannerQueries++;
        return scannerRunning;
      }

      void SignalWarning(ArmingStep step, int result) override
      {
        LogEntry entry { Call::Warning, currentState(), step, result };
        log.push_back(entry);
      }

      ArmState currentState() const { return (sequence != nullptr) ? sequence->State() : ArmState::Inactive; }

      void record(Call call)
      {
        LogEntry entry { call, currentState(), ArmingStep::DisablePins, 0 };
        log.push_back(entry);
      }

      // A disarm command handled from inside a callback - the re-entrancy the
      // sequence documents - optionally followed by a fresh Arm.
      void nestedDisarm()
      {
        nestedCancelled = sequence->Disarm();
        if (rearmAfterNestedDisarm) { assert(sequence->BeginArming(nestedNowMs)); }
      }

      ArmingSequence* sequence { nullptr };
      bool disarmInsideRestartArmed { false };
      bool disarmInsideEnable { false };
      bool rearmAfterNestedDisarm { false };
      bool nestedCancelled { false };
      int64_t nestedNowMs { 0 };
      std::vector<LogEntry> log;
      bool forbidActiveOnDisable { false };
      int disableResult { 0 };
      int restartArmedResult { 0 };
      int enableResult { 0 };
      bool scannerRunning { true };
      mutable int scannerQueries { 0 };
  };

  struct Fixture
  {
      Fixture()
          : sequence(actions)
      {
        actions.sequence = &sequence;
      }

      FakeActions actions;
      ArmingSequence sequence;
  };

  bool logContains(const FakeActions& actions, Call call)
  {
    for (const LogEntry& entry : actions.log) {
      if (entry.call == call) { return true; }
    }
    return false;
  }

  bool lastPinCallIsDisable(const FakeActions& actions)
  {
    for (auto entry = actions.log.rbegin(); entry != actions.log.rend(); ++entry) {
      if (entry->call == Call::EnablePins) { return false; }
      if (entry->call == Call::DisablePins) { return true; }
    }
    return false;
  }

  // Walks a fixture to Active through a clean exit delay, then clears the log.
  void armFully(Fixture& fixture, int64_t startMs)
  {
    assert(fixture.sequence.BeginArming(startMs));
    assert(fixture.sequence.Service(startMs + M_ARMING_DELAY_MS));
    assert(fixture.sequence.State() == ArmState::Active);
    fixture.actions.log.clear();
  }

  // 1. Nothing for 10 s, then exactly restart(true) -> enable -> Active.
  void testArmAfterExitDelay()
  {
    constexpr int64_t M_OFFSETS_BEFORE_DEADLINE[] { 0, M_ONE_MS, M_ARMING_DELAY_MS - M_ONE_MS };
    Fixture fixture;
    ArmingStep step { ArmingStep::DisablePins };
    int result { 0 };

    assert(fixture.sequence.State() == ArmState::Inactive);
    assert(fixture.sequence.BeginArming(M_START_MS));
    assert(fixture.sequence.State() == ArmState::Arming);
    assert(fixture.actions.log.empty());

    for (int64_t offsetMs : M_OFFSETS_BEFORE_DEADLINE) {
      assert(!fixture.sequence.Service(M_START_MS + offsetMs));
      assert(fixture.sequence.State() == ArmState::Arming);
      assert(fixture.actions.log.empty());
    }

    assert(fixture.sequence.Service(M_START_MS + M_ARMING_DELAY_MS));
    assert(fixture.sequence.State() == ArmState::Active);
    assert(fixture.actions.log.size() == 2);
    assert(fixture.actions.log[0].call == Call::RestartArmed);
    assert(fixture.actions.log[0].stateAtCall == ArmState::Arming);
    assert(fixture.actions.log[1].call == Call::EnablePins);
    assert(fixture.actions.log[1].stateAtCall == ArmState::Arming);

    // True once only: later services while Active do nothing and return false.
    assert(!fixture.sequence.Service(M_START_MS + M_ARMING_DELAY_MS + M_ONE_MS));
    assert(!fixture.sequence.Service(M_START_MS + M_WELL_PAST_MS));
    assert(fixture.actions.log.size() == 2);
    assert(fixture.sequence.State() == ArmState::Active);
    assert(!fixture.sequence.TakeFailure(step, result));

    printf("arming sequence: arm after exit delay: OK\n");
  }

  // 2. Section 2: disarm from Active disables the pins before any state change.
  void testDisarmFromActive()
  {
    Fixture fixture;
    ArmingStep step { ArmingStep::DisablePins };
    int result { 0 };

    armFully(fixture, M_START_MS);
    assert(!fixture.sequence.Disarm());
    assert(fixture.sequence.State() == ArmState::Inactive);
    assert(fixture.actions.log.size() == 2);
    assert(fixture.actions.log[0].call == Call::DisablePins);
    assert(fixture.actions.log[0].stateAtCall == ArmState::Active);
    assert(fixture.actions.log[1].call == Call::RestartDisarmed);
    assert(fixture.actions.log[1].stateAtCall == ArmState::Inactive);
    assert(!fixture.sequence.TakeFailure(step, result));

    printf("arming sequence: disarm from Active: OK\n");
  }

  // 3. Disarm at 0 s, mid-delay and at the last tick before 10 s cancels arming
  // and never reaches restart(true) or enable.
  void testDisarmDuringArming()
  {
    constexpr int64_t M_CANCEL_OFFSETS[] { 0, M_MID_DELAY_MS, M_ARMING_DELAY_MS - M_ONE_MS };

    for (int64_t offsetMs : M_CANCEL_OFFSETS) {
      Fixture fixture;

      assert(fixture.sequence.BeginArming(M_START_MS));
      assert(!fixture.sequence.Service(M_START_MS + offsetMs));
      assert(fixture.sequence.Disarm());
      assert(fixture.sequence.State() == ArmState::Inactive);
      assert(fixture.actions.log.size() == 2);
      assert(fixture.actions.log[0].call == Call::DisablePins);
      assert(fixture.actions.log[0].stateAtCall == ArmState::Arming);
      assert(fixture.actions.log[1].call == Call::RestartDisarmed);
      assert(fixture.actions.log[1].stateAtCall == ArmState::Inactive);

      assert(!fixture.sequence.Service(M_START_MS + M_ARMING_DELAY_MS));
      assert(!fixture.sequence.Service(M_START_MS + M_WELL_PAST_MS));
      assert(fixture.sequence.State() == ArmState::Inactive);
      assert(!logContains(fixture.actions, Call::RestartArmed));
      assert(!logContains(fixture.actions, Call::EnablePins));
      assert(fixture.actions.log.size() == 2);
    }

    printf("arming sequence: disarm during Arming cancels: OK\n");
  }

  // 4. Section 3 step 3: a restart failure fails safe and never enables.
  void testRestartFailure()
  {
    Fixture fixture;
    ArmingStep step { ArmingStep::DisablePins };
    int result { 0 };

    fixture.actions.restartArmedResult    = M_RESTART_FAILURE;
    fixture.actions.forbidActiveOnDisable = true;
    assert(fixture.sequence.BeginArming(M_START_MS));
    assert(!fixture.sequence.Service(M_START_MS + M_ARMING_DELAY_MS));
    assert(fixture.sequence.State() == ArmState::Inactive);
    assert(!logContains(fixture.actions, Call::EnablePins));
    assert(fixture.actions.log.size() == 4);
    assert(fixture.actions.log[0].call == Call::RestartArmed);
    assert(fixture.actions.log[1].call == Call::DisablePins);
    assert(fixture.actions.log[2].call == Call::RestartDisarmed);
    assert(fixture.actions.log[2].stateAtCall == ArmState::Inactive);
    assert(fixture.actions.log[3].call == Call::Warning);
    assert(fixture.actions.log[3].step == ArmingStep::RestartDetection);
    assert(fixture.actions.log[3].result == M_RESTART_FAILURE);

    assert(fixture.sequence.TakeFailure(step, result));
    assert(step == ArmingStep::RestartDetection && result == M_RESTART_FAILURE);
    assert(!fixture.sequence.TakeFailure(step, result));

    // No retry of its own: the next service does nothing.
    assert(!fixture.sequence.Service(M_START_MS + M_WELL_PAST_MS));
    assert(fixture.actions.log.size() == 4);

    printf("arming sequence: restart failure fails safe: OK\n");
  }

  // 5. An enable failure fails safe and the state is never Active at any point -
  // the fake asserts State() != Active inside every restart and enable call.
  void testEnableFailure()
  {
    Fixture fixture;
    ArmingStep step { ArmingStep::DisablePins };
    int result { 0 };

    fixture.actions.enableResult          = M_ENABLE_FAILURE;
    fixture.actions.forbidActiveOnDisable = true;
    assert(fixture.sequence.BeginArming(M_START_MS));
    assert(!fixture.sequence.Service(M_START_MS + M_ARMING_DELAY_MS));
    assert(fixture.sequence.State() == ArmState::Inactive);
    assert(fixture.actions.log.size() == 5);
    assert(fixture.actions.log[0].call == Call::RestartArmed);
    assert(fixture.actions.log[1].call == Call::EnablePins);
    assert(fixture.actions.log[2].call == Call::DisablePins);
    assert(fixture.actions.log[3].call == Call::RestartDisarmed);
    assert(fixture.actions.log[4].call == Call::Warning);
    assert(fixture.actions.log[4].step == ArmingStep::EnablePins);
    assert(fixture.actions.log[4].result == M_ENABLE_FAILURE);

    for (const LogEntry& entry : fixture.actions.log) {
      assert(entry.stateAtCall != ArmState::Active);
    }

    assert(fixture.sequence.TakeFailure(step, result));
    assert(step == ArmingStep::EnablePins && result == M_ENABLE_FAILURE);

    printf("arming sequence: enable failure never Active: OK\n");
  }

  // 6. A disable failure during Disarm still completes the disarm, then warns.
  void testDisableFailureDuringDisarm()
  {
    Fixture fixture;
    ArmingStep step { ArmingStep::RestartDetection };
    int result { 0 };

    armFully(fixture, M_START_MS);
    fixture.actions.disableResult = M_DISABLE_FAILURE;
    assert(!fixture.sequence.Disarm());
    assert(fixture.sequence.State() == ArmState::Inactive);
    assert(fixture.actions.log.size() == 3);
    assert(fixture.actions.log[0].call == Call::DisablePins);
    assert(fixture.actions.log[0].stateAtCall == ArmState::Active);
    assert(fixture.actions.log[1].call == Call::RestartDisarmed);
    assert(fixture.actions.log[1].stateAtCall == ArmState::Inactive);
    assert(fixture.actions.log[2].call == Call::Warning);
    assert(fixture.actions.log[2].step == ArmingStep::DisablePins);
    assert(fixture.actions.log[2].result == M_DISABLE_FAILURE);

    assert(fixture.sequence.TakeFailure(step, result));
    assert(step == ArmingStep::DisablePins && result == M_DISABLE_FAILURE);
    assert(!fixture.sequence.TakeFailure(step, result));

    printf("arming sequence: disable failure during disarm: OK\n");
  }

  // Both the arming step and the fail-safe disable fail: both warnings, but
  // TakeFailure reports DisablePins - the pins may not be isolated, which
  // outranks the step that failed.
  void testDisableFailureDuringFailSafe()
  {
    Fixture fixture;
    ArmingStep step { ArmingStep::DisablePins };
    int result { 0 };

    fixture.actions.enableResult  = M_ENABLE_FAILURE;
    fixture.actions.disableResult = M_DISABLE_FAILURE;
    assert(fixture.sequence.BeginArming(M_START_MS));
    assert(!fixture.sequence.Service(M_START_MS + M_ARMING_DELAY_MS));
    assert(fixture.sequence.State() == ArmState::Inactive);
    assert(fixture.actions.log.size() == 6);
    assert(fixture.actions.log[2].call == Call::DisablePins);
    assert(fixture.actions.log[3].call == Call::RestartDisarmed);
    assert(fixture.actions.log[4].call == Call::Warning && fixture.actions.log[4].step == ArmingStep::DisablePins);
    assert(fixture.actions.log[5].call == Call::Warning && fixture.actions.log[5].step == ArmingStep::EnablePins);

    for (const LogEntry& entry : fixture.actions.log) {
      assert(entry.stateAtCall != ArmState::Active);
    }

    assert(fixture.sequence.TakeFailure(step, result));
    assert(step == ArmingStep::DisablePins && result == M_DISABLE_FAILURE);
    assert(!fixture.sequence.TakeFailure(step, result));

    printf("arming sequence: disable failure during fail-safe: OK\n");
  }

  // RE-ENTRANCY: a Disarm() handled inside RestartDetection(true) wins. Service
  // never enables, never goes Active and raises no warning - the disarm was the
  // engineer's, not a failure - even if the armed restart then reports failure.
  void testDisarmInsideArmedRestart()
  {
    constexpr int M_RESTART_RESULTS[] { 0, M_RESTART_FAILURE };

    for (int restartResult : M_RESTART_RESULTS) {
      Fixture fixture;
      ArmingStep step { ArmingStep::DisablePins };
      int result { 0 };

      fixture.actions.restartArmedResult       = restartResult;
      fixture.actions.disarmInsideRestartArmed = true;
      assert(fixture.sequence.BeginArming(M_START_MS));
      assert(!fixture.sequence.Service(M_START_MS + M_ARMING_DELAY_MS));
      assert(fixture.actions.nestedCancelled);
      assert(fixture.sequence.State() == ArmState::Inactive);
      assert(fixture.actions.log.size() == 3);
      assert(fixture.actions.log[0].call == Call::RestartArmed);
      assert(fixture.actions.log[1].call == Call::DisablePins);
      assert(fixture.actions.log[2].call == Call::RestartDisarmed);
      assert(!logContains(fixture.actions, Call::EnablePins));
      assert(!logContains(fixture.actions, Call::Warning));
      assert(lastPinCallIsDisable(fixture.actions));
      assert(!fixture.sequence.TakeFailure(step, result));

      assert(!fixture.sequence.Service(M_START_MS + M_WELL_PAST_MS));
      assert(fixture.sequence.State() == ArmState::Inactive);
      assert(fixture.actions.log.size() == 3);
    }

    printf("arming sequence: disarm inside the armed restart: OK\n");
  }

  // RE-ENTRANCY: a Disarm() handled inside EnableFirePins(). The enable may have
  // configured the pins after the nested disable, so Service disables them again:
  // the last pin action is a disable, the state is Inactive, never Active, and
  // no warning is raised.
  void testDisarmInsideEnable()
  {
    Fixture fixture;
    ArmingStep step { ArmingStep::DisablePins };
    int result { 0 };

    fixture.actions.disarmInsideEnable = true;
    assert(fixture.sequence.BeginArming(M_START_MS));
    assert(!fixture.sequence.Service(M_START_MS + M_ARMING_DELAY_MS));
    assert(fixture.actions.nestedCancelled);
    assert(fixture.sequence.State() == ArmState::Inactive);
    assert(fixture.actions.log.size() == 5);
    assert(fixture.actions.log[0].call == Call::RestartArmed);
    assert(fixture.actions.log[1].call == Call::EnablePins);
    assert(fixture.actions.log[2].call == Call::DisablePins);
    assert(fixture.actions.log[3].call == Call::RestartDisarmed);
    assert(fixture.actions.log[4].call == Call::DisablePins);
    assert(fixture.actions.log[4].stateAtCall == ArmState::Inactive);
    assert(!logContains(fixture.actions, Call::Warning));
    assert(lastPinCallIsDisable(fixture.actions));
    assert(!fixture.sequence.TakeFailure(step, result));

    for (const LogEntry& entry : fixture.actions.log) {
      assert(entry.stateAtCall != ArmState::Active);
    }

    assert(!fixture.sequence.Service(M_START_MS + M_WELL_PAST_MS));
    assert(fixture.sequence.State() == ArmState::Inactive);
    assert(fixture.actions.log.size() == 5);

    printf("arming sequence: disarm inside the enable: OK\n");
  }

  // RE-ENTRANCY: Disarm() then a fresh BeginArming() inside a callback. The state
  // is Arming again, but it is a NEW arming with its own deadline: the interrupted
  // Service must not carry on and arm on the old one.
  void testDisarmAndRearmInsideCallback()
  {
    constexpr int64_t M_REARM_OFFSET_MS { 3000 };

    for (bool insideEnable : M_BOTH_CALLBACKS) {
      Fixture fixture;
      int64_t deadlineMs { M_START_MS + M_ARMING_DELAY_MS };
      int64_t rearmMs { deadlineMs + M_REARM_OFFSET_MS };

      fixture.actions.disarmInsideRestartArmed = !insideEnable;
      fixture.actions.disarmInsideEnable       = insideEnable;
      fixture.actions.rearmAfterNestedDisarm   = true;
      fixture.actions.nestedNowMs              = rearmMs;
      assert(fixture.sequence.BeginArming(M_START_MS));
      assert(!fixture.sequence.Service(deadlineMs));
      assert(fixture.sequence.State() == ArmState::Arming);
      assert(fixture.actions.log.size() == (insideEnable ? 5 : 3));
      assert(!logContains(fixture.actions, Call::Warning));
      assert(lastPinCallIsDisable(fixture.actions));

      for (const LogEntry& entry : fixture.actions.log) {
        assert(entry.stateAtCall != ArmState::Active);
      }

      // The new arming runs its own full exit delay, then arms normally.
      fixture.actions.log.clear();
      assert(!fixture.sequence.Service(rearmMs + M_ARMING_DELAY_MS - M_ONE_MS));
      assert(fixture.actions.log.empty());
      assert(fixture.sequence.Service(rearmMs + M_ARMING_DELAY_MS));
      assert(fixture.sequence.State() == ArmState::Active);
      assert(fixture.actions.log.size() == 2);
    }

    printf("arming sequence: disarm and re-arm inside a callback: OK\n");
  }

  // 7. BeginArming is ignored unless Inactive, and never moves the deadline.
  void testBeginArmingIgnoredUnlessInactive()
  {
    Fixture fixture;

    assert(fixture.sequence.BeginArming(M_START_MS));
    assert(!fixture.sequence.BeginArming(M_START_MS + M_MID_DELAY_MS));
    assert(fixture.sequence.State() == ArmState::Arming);

    // The original deadline stands: not before it, exactly at it.
    assert(!fixture.sequence.Service(M_START_MS + M_ARMING_DELAY_MS - M_ONE_MS));
    assert(fixture.sequence.Service(M_START_MS + M_ARMING_DELAY_MS));
    assert(fixture.sequence.State() == ArmState::Active);
    fixture.actions.log.clear();

    assert(!fixture.sequence.BeginArming(M_START_MS + M_WELL_PAST_MS));
    assert(fixture.sequence.State() == ArmState::Active);
    assert(!fixture.sequence.Service(M_START_MS + M_WELL_PAST_MS + M_ARMING_DELAY_MS));
    assert(fixture.actions.log.empty());

    printf("arming sequence: BeginArming ignored unless Inactive: OK\n");
  }

  // 8. Disarm from Inactive still disables the pins first.
  void testDisarmFromInactive()
  {
    Fixture fixture;

    assert(!fixture.sequence.Disarm());
    assert(fixture.sequence.State() == ArmState::Inactive);
    assert(fixture.actions.log.size() == 2);
    assert(fixture.actions.log[0].call == Call::DisablePins);
    assert(fixture.actions.log[1].call == Call::RestartDisarmed);

    // And a fresh arm afterwards still works.
    fixture.actions.log.clear();
    assert(fixture.sequence.BeginArming(M_START_MS));
    assert(fixture.sequence.Service(M_START_MS + M_ARMING_DELAY_MS));
    assert(fixture.sequence.State() == ArmState::Active);

    printf("arming sequence: disarm from Inactive: OK\n");
  }

  // 9. No scanner, no arming: an Arm while the scanner is down is refused
  // outright - still Inactive, no delay started, nothing but the warning.
  void testBeginArmingRefusedWithoutScanner()
  {
    Fixture fixture;
    ArmingStep step { ArmingStep::DisablePins };
    int result { 0 };

    fixture.actions.scannerRunning = false;
    assert(!fixture.sequence.BeginArming(M_START_MS));
    assert(fixture.sequence.State() == ArmState::Inactive);
    assert(fixture.actions.log.size() == 1);
    assert(fixture.actions.log[0].call == Call::Warning);
    assert(fixture.actions.log[0].stateAtCall == ArmState::Inactive);
    assert(fixture.actions.log[0].step == ArmingStep::ScannerCheck);
    assert(fixture.actions.log[0].result == M_SCANNER_FAILURE);

    assert(fixture.sequence.TakeFailure(step, result));
    assert(step == ArmingStep::ScannerCheck && result == M_SCANNER_FAILURE);
    assert(!fixture.sequence.TakeFailure(step, result));

    // No deadline was started: servicing well past it does nothing.
    assert(!fixture.sequence.Service(M_START_MS + M_WELL_PAST_MS));
    assert(fixture.sequence.State() == ArmState::Inactive);
    assert(fixture.actions.log.size() == 1);

    // Once the scanner is back, a fresh arm works normally.
    fixture.actions.scannerRunning = true;
    fixture.actions.log.clear();
    assert(fixture.sequence.BeginArming(M_START_MS));
    assert(fixture.sequence.Service(M_START_MS + M_ARMING_DELAY_MS));
    assert(fixture.sequence.State() == ArmState::Active);

    // Arming or Active, BeginArming is still ignored without a warning.
    fixture.actions.scannerRunning = false;
    fixture.actions.log.clear();
    assert(!fixture.sequence.BeginArming(M_START_MS + M_WELL_PAST_MS));
    assert(fixture.actions.log.empty());

    printf("arming sequence: BeginArming refused without scanner: OK\n");
  }

  // 10. The scanner stops during the exit delay: at the deadline the sequence
  // fails safe before the armed restart - never enables, never Active.
  void testScannerDownAtDeadline()
  {
    Fixture fixture;
    ArmingStep step { ArmingStep::DisablePins };
    int result { 0 };

    fixture.actions.forbidActiveOnDisable = true;
    assert(fixture.sequence.BeginArming(M_START_MS));
    fixture.actions.scannerRunning = false;

    // Before the deadline nothing happens, scanner or not.
    assert(!fixture.sequence.Service(M_START_MS + M_MID_DELAY_MS));
    assert(fixture.sequence.State() == ArmState::Arming);
    assert(fixture.actions.log.empty());

    assert(!fixture.sequence.Service(M_START_MS + M_ARMING_DELAY_MS));
    assert(fixture.sequence.State() == ArmState::Inactive);
    assert(!logContains(fixture.actions, Call::RestartArmed));
    assert(!logContains(fixture.actions, Call::EnablePins));
    assert(fixture.actions.log.size() == 3);
    assert(fixture.actions.log[0].call == Call::DisablePins);
    assert(fixture.actions.log[0].stateAtCall == ArmState::Arming);
    assert(fixture.actions.log[1].call == Call::RestartDisarmed);
    assert(fixture.actions.log[1].stateAtCall == ArmState::Inactive);
    assert(fixture.actions.log[2].call == Call::Warning);
    assert(fixture.actions.log[2].step == ArmingStep::ScannerCheck);
    assert(fixture.actions.log[2].result == M_SCANNER_FAILURE);

    for (const LogEntry& entry : fixture.actions.log) {
      assert(entry.stateAtCall != ArmState::Active);
    }

    assert(fixture.sequence.TakeFailure(step, result));
    assert(step == ArmingStep::ScannerCheck && result == M_SCANNER_FAILURE);

    // No retry of its own.
    assert(!fixture.sequence.Service(M_START_MS + M_WELL_PAST_MS));
    assert(fixture.actions.log.size() == 3);

    printf("arming sequence: scanner down at deadline fails safe: OK\n");
  }

  // 11. A scanner refusal does not overwrite a pending pin disable failure -
  // the pins may not be isolated, which outranks it.
  void testScannerRefusalKeepsDisableFailure()
  {
    Fixture fixture;
    ArmingStep step { ArmingStep::ScannerCheck };
    int result { 0 };

    fixture.actions.disableResult = M_DISABLE_FAILURE;
    assert(!fixture.sequence.Disarm());
    fixture.actions.disableResult  = 0;
    fixture.actions.scannerRunning = false;
    assert(!fixture.sequence.BeginArming(M_START_MS));
    assert(fixture.actions.log.back().call == Call::Warning);
    assert(fixture.actions.log.back().step == ArmingStep::ScannerCheck);

    assert(fixture.sequence.TakeFailure(step, result));
    assert(step == ArmingStep::DisablePins && result == M_DISABLE_FAILURE);
    assert(!fixture.sequence.TakeFailure(step, result));

    printf("arming sequence: scanner refusal keeps a disable failure: OK\n");
  }

  // 12. The scanner is queried at BeginArming and again at the deadline - a
  // scanner that stops during the exit delay must be caught before arming.
  void testScannerQueriedAtBeginAndDeadline()
  {
    Fixture fixture;

    assert(fixture.sequence.BeginArming(M_START_MS));
    int queriesAfterBegin { fixture.actions.scannerQueries };
    assert(queriesAfterBegin >= 1);
    assert(fixture.sequence.Service(M_START_MS + M_ARMING_DELAY_MS));
    assert(fixture.actions.scannerQueries > queriesAfterBegin);

    printf("arming sequence: scanner queried at begin and deadline: OK\n");
  }

}

void run_arming_sequence_tests()
{
  testArmAfterExitDelay();
  testDisarmFromActive();
  testDisarmDuringArming();
  testRestartFailure();
  testEnableFailure();
  testDisableFailureDuringDisarm();
  testDisableFailureDuringFailSafe();
  testDisarmInsideArmedRestart();
  testDisarmInsideEnable();
  testDisarmAndRearmInsideCallback();
  testBeginArmingIgnoredUnlessInactive();
  testDisarmFromInactive();
  testBeginArmingRefusedWithoutScanner();
  testScannerDownAtDeadline();
  testScannerRefusalKeepsDisableFailure();
  testScannerQueriedAtBeginAndDeadline();
  printf("arming sequence: OK\n");
}
