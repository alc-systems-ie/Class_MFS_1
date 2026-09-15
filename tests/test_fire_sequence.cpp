#include <cassert>
#include <cstdio>

#include "fire_sequence.hpp"

// Host tests for FireSequence. Plan Task 5,
// docs/superpowers/plans/2026-09-15-fire-command.md.

namespace
{

  using namespace alc;

  constexpr uint32_t M_START_MS { 0 };
  constexpr uint32_t M_COUNTDOWN_MS { FireSequence::M_FIRE_COUNTDOWN_MS };
  constexpr uint32_t M_ONE_MS { 1 };
  constexpr uint32_t M_MID_COUNTDOWN_MS { 5000 };
  constexpr uint32_t M_WELL_PAST_MS { 20000 };

  // 1. A live fire sets FireLatched() only at or after the 10 s deadline.
  void testLiveFiresAfter10Seconds()
  {
    FireSequence sequence;

    sequence.Start(/*live=*/true, M_START_MS);
    assert(sequence.State() == FireState::CountingDown);

    sequence.Service(M_COUNTDOWN_MS - M_ONE_MS, false);
    assert(!sequence.FireLatched());
    assert(sequence.State() == FireState::CountingDown);

    sequence.Service(M_COUNTDOWN_MS, false);
    assert(sequence.FireLatched());
    assert(sequence.State() == FireState::Fired);
    assert(!sequence.RehearsalFired());
    assert(!sequence.Aborted());

    printf("fire sequence: live fires after 10 s: OK\n");
  }

  // 2. AcceptsDisarm() is false for the whole countdown - the fire is
  // unstoppable by a Disarm command.
  void testDisarmRefusedDuringCountdown()
  {
    FireSequence sequence;

    sequence.Start(true, M_START_MS);
    assert(!sequence.AcceptsDisarm());

    sequence.Service(M_MID_COUNTDOWN_MS, false);
    assert(!sequence.AcceptsDisarm());

    sequence.Service(M_COUNTDOWN_MS, false);
    assert(sequence.State() == FireState::Fired);
    assert(sequence.AcceptsDisarm());

    printf("fire sequence: disarm refused during countdown: OK\n");
  }

  // 3. NO SCANNER INPUT AT ALL: the class has nothing that could be told the
  // scanner is down, so a scanner loss can never abort the countdown - the
  // exemption is proven by the class's shape, not by a stubbed dependency.
  void testScannerLossDoesNotAbort()
  {
    FireSequence sequence;

    sequence.Start(true, M_START_MS);
    sequence.Service(M_COUNTDOWN_MS, false);
    assert(sequence.FireLatched());
    assert(!sequence.Aborted());

    printf("fire sequence: scanner loss does not abort (no scanner input exists): OK\n");
  }

  // 4. A fire-switch fault mid-countdown aborts at once and the countdown
  // never fires afterwards, however far nowMs then advances.
  void testFireSwitchFaultAbortsAndFailsSafe()
  {
    FireSequence sequence;

    sequence.Start(true, M_START_MS);
    sequence.Service(M_MID_COUNTDOWN_MS, /*fireSwitchFaulty=*/true);
    assert(sequence.Aborted());
    assert(!sequence.FireLatched());
    assert(sequence.State() == FireState::Idle);

    sequence.Service(M_COUNTDOWN_MS, false);
    assert(!sequence.FireLatched());
    assert(sequence.State() == FireState::Idle);

    sequence.Service(M_WELL_PAST_MS, false);
    assert(!sequence.FireLatched());

    printf("fire sequence: fire switch fault aborts and fails safe: OK\n");
  }

  // 5. A rehearsal (live = false) lights RehearsalFired() only, never the fire
  // pins' FireLatched().
  void testRehearsalLightsLedBNotPins()
  {
    FireSequence sequence;

    sequence.Start(/*live=*/false, M_START_MS);
    sequence.Service(M_COUNTDOWN_MS, false);
    assert(!sequence.FireLatched());
    assert(sequence.RehearsalFired());
    assert(sequence.State() == FireState::Fired);

    printf("fire sequence: rehearsal lights LED B not the fire pins: OK\n");
  }

  // 6. One-shot: once Fired, later Service() calls do nothing - no re-arm.
  void testOneShot()
  {
    FireSequence sequence;

    sequence.Start(true, M_START_MS);
    sequence.Service(M_COUNTDOWN_MS, false);
    assert(sequence.FireLatched());

    sequence.Service(M_WELL_PAST_MS, false);
    assert(sequence.State() == FireState::Fired);
    assert(sequence.FireLatched());

    // Not even a fire-switch fault after firing changes anything: Service()
    // is a no-op once the state has left CountingDown.
    sequence.Service(M_WELL_PAST_MS + M_ONE_MS, /*fireSwitchFaulty=*/true);
    assert(sequence.State() == FireState::Fired);
    assert(!sequence.Aborted());

    printf("fire sequence: one shot, no re-arm: OK\n");
  }

  // 7. Idle before Start() - Service() is a no-op and every latch is clear.
  void testIdleBeforeStart()
  {
    FireSequence sequence;

    assert(sequence.State() == FireState::Idle);
    assert(sequence.AcceptsDisarm());
    sequence.Service(M_COUNTDOWN_MS, false);
    assert(sequence.State() == FireState::Idle);
    assert(!sequence.FireLatched());
    assert(!sequence.RehearsalFired());
    assert(!sequence.Aborted());

    printf("fire sequence: idle before Start does nothing: OK\n");
  }

  // 8. A fresh Start() after a completed fire clears every latch from the
  // previous countdown, so a second rehearsal is seen as its own event.
  void testStartAfterFiredClearsLatches()
  {
    FireSequence sequence;

    sequence.Start(false, M_START_MS);
    sequence.Service(M_COUNTDOWN_MS, false);
    assert(sequence.RehearsalFired());

    sequence.Start(false, M_COUNTDOWN_MS);
    assert(sequence.State() == FireState::CountingDown);
    assert(!sequence.RehearsalFired());
    assert(!sequence.AcceptsDisarm());

    sequence.Service(M_COUNTDOWN_MS + M_COUNTDOWN_MS, false);
    assert(sequence.RehearsalFired());
    assert(!sequence.FireLatched());

    printf("fire sequence: Start after Fired clears latches for a new countdown: OK\n");
  }

  // 9. A fresh Start() after an abort clears Aborted() and counts down again.
  void testStartAfterAbortClearsAborted()
  {
    FireSequence sequence;

    sequence.Start(true, M_START_MS);
    sequence.Service(M_MID_COUNTDOWN_MS, true);
    assert(sequence.Aborted());

    sequence.Start(true, M_MID_COUNTDOWN_MS);
    assert(!sequence.Aborted());
    assert(sequence.State() == FireState::CountingDown);

    sequence.Service(M_MID_COUNTDOWN_MS + M_COUNTDOWN_MS, false);
    assert(sequence.FireLatched());

    printf("fire sequence: Start after abort clears Aborted: OK\n");
  }

}

void run_fire_sequence_tests()
{
  testLiveFiresAfter10Seconds();
  testDisarmRefusedDuringCountdown();
  testScannerLossDoesNotAbort();
  testFireSwitchFaultAbortsAndFailsSafe();
  testRehearsalLightsLedBNotPins();
  testOneShot();
  testIdleBeforeStart();
  testStartAfterFiredClearsLatches();
  testStartAfterAbortClearsAborted();
  printf("fire sequence: OK\n");
}
