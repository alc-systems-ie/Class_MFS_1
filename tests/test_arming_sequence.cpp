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
  constexpr int64_t M_EXIT_DELAY_MS { ArmingSequence::M_EXIT_DELAY_MS };
  constexpr int64_t M_ONE_MS { 1 };
  constexpr int64_t M_MID_DELAY_MS { 5000 };
  constexpr int64_t M_WELL_PAST_MS { 60000 };
  constexpr int M_RESTART_FAILURE { -EIO };
  constexpr int M_ENABLE_FAILURE { -ENODEV };
  constexpr int M_DISABLE_FAILURE { -EBUSY };

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
        return armed ? restartArmedResult : 0;
      }

      int EnableFirePins() override
      {
        record(Call::EnablePins);
        // Section 3 step 2.3: Active is set only AFTER the enable returned 0.
        if (sequence != nullptr) { assert(sequence->State() != ArmState::Active); }
        return enableResult;
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

      ArmingSequence* sequence { nullptr };
      std::vector<LogEntry> log;
      bool forbidActiveOnDisable { false };
      int disableResult { 0 };
      int restartArmedResult { 0 };
      int enableResult { 0 };
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

  // Walks a fixture to Active through a clean exit delay, then clears the log.
  void armFully(Fixture& fixture, int64_t startMs)
  {
    assert(fixture.sequence.BeginArming(startMs));
    assert(fixture.sequence.Service(startMs + M_EXIT_DELAY_MS));
    assert(fixture.sequence.State() == ArmState::Active);
    fixture.actions.log.clear();
  }

  // 1. Nothing for 10 s, then exactly restart(true) -> enable -> Active.
  void testArmAfterExitDelay()
  {
    constexpr int64_t M_OFFSETS_BEFORE_DEADLINE[] { 0, M_ONE_MS, M_EXIT_DELAY_MS - M_ONE_MS };
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

    assert(fixture.sequence.Service(M_START_MS + M_EXIT_DELAY_MS));
    assert(fixture.sequence.State() == ArmState::Active);
    assert(fixture.actions.log.size() == 2);
    assert(fixture.actions.log[0].call == Call::RestartArmed);
    assert(fixture.actions.log[0].stateAtCall == ArmState::Arming);
    assert(fixture.actions.log[1].call == Call::EnablePins);
    assert(fixture.actions.log[1].stateAtCall == ArmState::Arming);

    // True once only: later services while Active do nothing and return false.
    assert(!fixture.sequence.Service(M_START_MS + M_EXIT_DELAY_MS + M_ONE_MS));
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
    constexpr int64_t M_CANCEL_OFFSETS[] { 0, M_MID_DELAY_MS, M_EXIT_DELAY_MS - M_ONE_MS };

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

      assert(!fixture.sequence.Service(M_START_MS + M_EXIT_DELAY_MS));
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
    assert(!fixture.sequence.Service(M_START_MS + M_EXIT_DELAY_MS));
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
    assert(!fixture.sequence.Service(M_START_MS + M_EXIT_DELAY_MS));
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

  // Both the arming step and the fail-safe disable fail: both warnings, the
  // arming step last, and it is the failure TakeFailure reports.
  void testDisableFailureDuringFailSafe()
  {
    Fixture fixture;
    ArmingStep step { ArmingStep::DisablePins };
    int result { 0 };

    fixture.actions.enableResult  = M_ENABLE_FAILURE;
    fixture.actions.disableResult = M_DISABLE_FAILURE;
    assert(fixture.sequence.BeginArming(M_START_MS));
    assert(!fixture.sequence.Service(M_START_MS + M_EXIT_DELAY_MS));
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
    assert(step == ArmingStep::EnablePins && result == M_ENABLE_FAILURE);

    printf("arming sequence: disable failure during fail-safe: OK\n");
  }

  // 7. BeginArming is ignored unless Inactive, and never moves the deadline.
  void testBeginArmingIgnoredUnlessInactive()
  {
    Fixture fixture;

    assert(fixture.sequence.BeginArming(M_START_MS));
    assert(!fixture.sequence.BeginArming(M_START_MS + M_MID_DELAY_MS));
    assert(fixture.sequence.State() == ArmState::Arming);

    // The original deadline stands: not before it, exactly at it.
    assert(!fixture.sequence.Service(M_START_MS + M_EXIT_DELAY_MS - M_ONE_MS));
    assert(fixture.sequence.Service(M_START_MS + M_EXIT_DELAY_MS));
    assert(fixture.sequence.State() == ArmState::Active);
    fixture.actions.log.clear();

    assert(!fixture.sequence.BeginArming(M_START_MS + M_WELL_PAST_MS));
    assert(fixture.sequence.State() == ArmState::Active);
    assert(!fixture.sequence.Service(M_START_MS + M_WELL_PAST_MS + M_EXIT_DELAY_MS));
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
    assert(fixture.sequence.Service(M_START_MS + M_EXIT_DELAY_MS));
    assert(fixture.sequence.State() == ArmState::Active);

    printf("arming sequence: disarm from Inactive: OK\n");
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
  testBeginArmingIgnoredUnlessInactive();
  testDisarmFromInactive();
  printf("arming sequence: OK\n");
}
