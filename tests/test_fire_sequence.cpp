#include <cassert>
#include <cstdio>

#include "fire_sequence.hpp"

// Host tests for FireSequence. Plan Task 5,
// docs/superpowers/plans/2026-09-15-fire-command.md; amended by Task 5b for
// int64_t time and the bounded Firing assertion window.

namespace
{

  using namespace alc;

  constexpr int64_t M_START_MS { 0 };
  constexpr int64_t M_COUNTDOWN_MS { FireSequence::M_FIRE_COUNTDOWN_MS };
  constexpr int64_t M_ONE_MS { 1 };
  constexpr int64_t M_MID_COUNTDOWN_MS { 5000 };
  constexpr int64_t M_WELL_PAST_MS { 20000 };
  constexpr int64_t M_ASSERT_MS { 500 };
  constexpr int64_t M_LARGE_START_MS { 5'000'000'000 };

  // 1. A live fire asserts FireLatched() only from the 10 s deadline through
  // the assertMs window, then drops and completes.
  void testLiveFiresAfter10SecondsThenDrops()
  {
    FireSequence sequence;

    sequence.Start(/*live=*/true, M_START_MS, M_ASSERT_MS);
    assert(sequence.State() == FireState::CountingDown);

    sequence.Service(M_COUNTDOWN_MS - M_ONE_MS, false);
    assert(!sequence.FireLatched());
    assert(sequence.State() == FireState::CountingDown);

    sequence.Service(M_COUNTDOWN_MS, false);
    assert(sequence.FireLatched());
    assert(sequence.State() == FireState::Firing);
    assert(!sequence.RehearsalFired());
    assert(!sequence.Aborted());

    // Still asserting partway through the window.
    sequence.Service(M_COUNTDOWN_MS + M_ASSERT_MS - M_ONE_MS, false);
    assert(sequence.FireLatched());
    assert(sequence.State() == FireState::Firing);

    // Window elapsed: drops and completes.
    sequence.Service(M_COUNTDOWN_MS + M_ASSERT_MS, false);
    assert(!sequence.FireLatched());
    assert(sequence.State() == FireState::Completed);

    printf("fire sequence: live fires after 10 s then drops after the assertion window: OK\n");
  }

  // 2. LIVE fire: AcceptsDisarm() is false for the whole countdown and the
  // whole assertion window - the fire event is unstoppable by a Disarm
  // command - and true again once Completed.
  void testDisarmRefusedUntilCompleted()
  {
    FireSequence sequence;

    sequence.Start(true, M_START_MS, M_ASSERT_MS);
    assert(!sequence.AcceptsDisarm());

    sequence.Service(M_MID_COUNTDOWN_MS, false);
    assert(!sequence.AcceptsDisarm());

    sequence.Service(M_COUNTDOWN_MS, false);
    assert(sequence.State() == FireState::Firing);
    assert(!sequence.AcceptsDisarm());

    sequence.Service(M_COUNTDOWN_MS + M_ASSERT_MS, false);
    assert(sequence.State() == FireState::Completed);
    assert(sequence.AcceptsDisarm());

    printf("fire sequence: disarm refused until completed: OK\n");
  }

  // 3. NO SCANNER INPUT AT ALL: the class has nothing that could be told the
  // scanner is down, so a scanner loss can never abort the countdown - the
  // exemption is proven by the class's shape, not by a stubbed dependency.
  void testScannerLossDoesNotAbort()
  {
    FireSequence sequence;

    sequence.Start(true, M_START_MS, M_ASSERT_MS);
    sequence.Service(M_COUNTDOWN_MS, false);
    assert(sequence.FireLatched());
    assert(!sequence.Aborted());

    printf("fire sequence: scanner loss does not abort (no scanner input exists): OK\n");
  }

  // 4. A fire-switch fault mid-countdown aborts at once and the countdown
  // never fires afterwards, however far nowMs then advances - FireLatched()
  // stays false through the whole would-be window.
  void testFireSwitchFaultDuringCountdownAbortsAndNeverFires()
  {
    FireSequence sequence;

    sequence.Start(true, M_START_MS, M_ASSERT_MS);
    sequence.Service(M_MID_COUNTDOWN_MS, /*fireSwitchFaulty=*/true);
    assert(sequence.Aborted());
    assert(!sequence.FireLatched());
    assert(sequence.State() == FireState::Idle);

    sequence.Service(M_COUNTDOWN_MS, false);
    assert(!sequence.FireLatched());
    assert(sequence.State() == FireState::Idle);

    sequence.Service(M_WELL_PAST_MS, false);
    assert(!sequence.FireLatched());
    assert(sequence.State() == FireState::Idle);

    printf("fire sequence: fire switch fault during countdown aborts and never fires: OK\n");
  }

  // 5. A fire-switch fault while Firing ends the window immediately -
  // Completed at once, FireLatched() then false.
  void testFireSwitchFaultDuringFiringEndsWindowImmediately()
  {
    FireSequence sequence;

    sequence.Start(true, M_START_MS, M_ASSERT_MS);
    sequence.Service(M_COUNTDOWN_MS, false);
    assert(sequence.FireLatched());
    assert(sequence.State() == FireState::Firing);

    sequence.Service(M_COUNTDOWN_MS + M_ONE_MS, /*fireSwitchFaulty=*/true);
    assert(sequence.State() == FireState::Completed);
    assert(!sequence.FireLatched());
    assert(!sequence.Aborted());

    printf("fire sequence: fire switch fault during firing ends the window immediately: OK\n");
  }

  // 6. A rehearsal (live = false) lights RehearsalFired() only for the
  // assertion window, never the fire pins' FireLatched(), and drops the same way.
  void testRehearsalLightsLedBNotPins()
  {
    FireSequence sequence;

    sequence.Start(/*live=*/false, M_START_MS, M_ASSERT_MS);
    sequence.Service(M_COUNTDOWN_MS, false);
    assert(!sequence.FireLatched());
    assert(sequence.RehearsalFired());
    assert(sequence.State() == FireState::Firing);

    sequence.Service(M_COUNTDOWN_MS + M_ASSERT_MS - M_ONE_MS, false);
    assert(sequence.RehearsalFired());
    assert(!sequence.FireLatched());

    sequence.Service(M_COUNTDOWN_MS + M_ASSERT_MS, false);
    assert(!sequence.RehearsalFired());
    assert(!sequence.FireLatched());
    assert(sequence.State() == FireState::Completed);

    printf("fire sequence: rehearsal lights LED B not the fire pins, then drops: OK\n");
  }

  // 7. One-shot: once Completed, later Service() calls do nothing - no
  // re-arm - but a fresh Start() begins again.
  void testOneShotThenFreshStart()
  {
    FireSequence sequence;

    sequence.Start(true, M_START_MS, M_ASSERT_MS);
    sequence.Service(M_COUNTDOWN_MS, false);
    sequence.Service(M_COUNTDOWN_MS + M_ASSERT_MS, false);
    assert(sequence.State() == FireState::Completed);
    assert(!sequence.FireLatched());

    // Not even a fire-switch fault after completing changes anything:
    // Service() is a no-op once the state has left CountingDown/Firing.
    sequence.Service(M_WELL_PAST_MS, /*fireSwitchFaulty=*/true);
    assert(sequence.State() == FireState::Completed);
    assert(!sequence.Aborted());

    // A fresh Start() begins an entirely new countdown.
    sequence.Start(true, M_WELL_PAST_MS, M_ASSERT_MS);
    assert(sequence.State() == FireState::CountingDown);
    sequence.Service(M_WELL_PAST_MS + M_COUNTDOWN_MS, false);
    assert(sequence.FireLatched());

    printf("fire sequence: one shot, no re-arm, fresh Start begins again: OK\n");
  }

  // 8. Idle before Start() - Service() is a no-op and every latch is clear.
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

  // 9. A fresh Start() after a completed fire clears every latch from the
  // previous countdown, so a second rehearsal is seen as its own event.
  void testStartAfterCompletedClearsLatches()
  {
    FireSequence sequence;

    sequence.Start(false, M_START_MS, M_ASSERT_MS);
    sequence.Service(M_COUNTDOWN_MS, false);
    assert(sequence.RehearsalFired());
    sequence.Service(M_COUNTDOWN_MS + M_ASSERT_MS, false);
    assert(sequence.State() == FireState::Completed);
    assert(!sequence.RehearsalFired());

    sequence.Start(false, M_COUNTDOWN_MS + M_ASSERT_MS, M_ASSERT_MS);
    assert(sequence.State() == FireState::CountingDown);
    assert(!sequence.RehearsalFired());
    // A rehearsal is interruptible throughout (amendment §4 scopes the
    // uninterruptible carve-out to a live fire only).
    assert(sequence.AcceptsDisarm());

    sequence.Service(M_COUNTDOWN_MS + M_ASSERT_MS + M_COUNTDOWN_MS, false);
    assert(sequence.RehearsalFired());
    assert(!sequence.FireLatched());

    printf("fire sequence: Start after Completed clears latches for a new countdown: OK\n");
  }

  // 10. A fresh Start() after an abort clears Aborted() and counts down again.
  void testStartAfterAbortClearsAborted()
  {
    FireSequence sequence;

    sequence.Start(true, M_START_MS, M_ASSERT_MS);
    sequence.Service(M_MID_COUNTDOWN_MS, true);
    assert(sequence.Aborted());

    sequence.Start(true, M_MID_COUNTDOWN_MS, M_ASSERT_MS);
    assert(!sequence.Aborted());
    assert(sequence.State() == FireState::CountingDown);

    sequence.Service(M_MID_COUNTDOWN_MS + M_COUNTDOWN_MS, false);
    assert(sequence.FireLatched());

    printf("fire sequence: Start after abort clears Aborted: OK\n");
  }

  // 12. A rehearsal is interruptible throughout: AcceptsDisarm() is true
  // during both a rehearsal countdown and rehearsal Firing (final review L2).
  void testAcceptsDisarmTrueDuringRehearsal()
  {
    FireSequence sequence;

    sequence.Start(/*live=*/false, M_START_MS, M_ASSERT_MS);
    assert(sequence.State() == FireState::CountingDown);
    assert(sequence.AcceptsDisarm());

    sequence.Service(M_MID_COUNTDOWN_MS, false);
    assert(sequence.AcceptsDisarm());

    sequence.Service(M_COUNTDOWN_MS, false);
    assert(sequence.State() == FireState::Firing);
    assert(sequence.RehearsalFired());
    assert(sequence.AcceptsDisarm());

    sequence.Service(M_COUNTDOWN_MS + M_ASSERT_MS, false);
    assert(sequence.State() == FireState::Completed);
    assert(sequence.AcceptsDisarm());

    printf("fire sequence: AcceptsDisarm true throughout a rehearsal countdown and firing: OK\n");
  }

  // 13. A live fire is uninterruptible: AcceptsDisarm() is false during a
  // live countdown and live Firing, and true once Idle or Completed
  // (final review §4 - restated alongside the rehearsal case above).
  void testAcceptsDisarmFalseDuringLiveFireOnly()
  {
    FireSequence sequence;

    assert(sequence.AcceptsDisarm());

    sequence.Start(/*live=*/true, M_START_MS, M_ASSERT_MS);
    assert(sequence.State() == FireState::CountingDown);
    assert(!sequence.AcceptsDisarm());

    sequence.Service(M_COUNTDOWN_MS, false);
    assert(sequence.State() == FireState::Firing);
    assert(sequence.FireLatched());
    assert(!sequence.AcceptsDisarm());

    sequence.Service(M_COUNTDOWN_MS + M_ASSERT_MS, false);
    assert(sequence.State() == FireState::Completed);
    assert(sequence.AcceptsDisarm());

    printf("fire sequence: AcceptsDisarm false only during a live countdown and live firing: OK\n");
  }

  // 14. LiveInProgress() is true only for a live CountingDown/Firing - false
  // for every rehearsal state, and false when Idle/Completed.
  void testLiveInProgress()
  {
    FireSequence live;
    FireSequence rehearsal;

    assert(!live.LiveInProgress());
    assert(!rehearsal.LiveInProgress());

    live.Start(/*live=*/true, M_START_MS, M_ASSERT_MS);
    rehearsal.Start(/*live=*/false, M_START_MS, M_ASSERT_MS);
    assert(live.LiveInProgress());
    assert(!rehearsal.LiveInProgress());

    live.Service(M_MID_COUNTDOWN_MS, false);
    rehearsal.Service(M_MID_COUNTDOWN_MS, false);
    assert(live.LiveInProgress());
    assert(!rehearsal.LiveInProgress());

    live.Service(M_COUNTDOWN_MS, false);
    rehearsal.Service(M_COUNTDOWN_MS, false);
    assert(live.State() == FireState::Firing);
    assert(rehearsal.State() == FireState::Firing);
    assert(live.LiveInProgress());
    assert(!rehearsal.LiveInProgress());

    live.Service(M_COUNTDOWN_MS + M_ASSERT_MS, false);
    rehearsal.Service(M_COUNTDOWN_MS + M_ASSERT_MS, false);
    assert(live.State() == FireState::Completed);
    assert(rehearsal.State() == FireState::Completed);
    assert(!live.LiveInProgress());
    assert(!rehearsal.LiveInProgress());

    printf("fire sequence: LiveInProgress true only for a live countdown/firing: OK\n");
  }

  // 15. int64_t: a large nowMs, past the old uint32_t wrap point (~49.7 days,
  // ~4.29e9 ms), still counts down and asserts correctly.
  void testLargeNowMsPastUint32Wrap()
  {
    FireSequence sequence;

    sequence.Start(true, M_LARGE_START_MS, M_ASSERT_MS);
    assert(sequence.State() == FireState::CountingDown);

    sequence.Service(M_LARGE_START_MS + M_COUNTDOWN_MS - M_ONE_MS, false);
    assert(!sequence.FireLatched());

    sequence.Service(M_LARGE_START_MS + M_COUNTDOWN_MS, false);
    assert(sequence.FireLatched());
    assert(sequence.State() == FireState::Firing);

    sequence.Service(M_LARGE_START_MS + M_COUNTDOWN_MS + M_ASSERT_MS, false);
    assert(!sequence.FireLatched());
    assert(sequence.State() == FireState::Completed);

    printf("fire sequence: large nowMs past the uint32 wrap still fires and completes correctly: OK\n");
  }

}

void run_fire_sequence_tests()
{
  testLiveFiresAfter10SecondsThenDrops();
  testDisarmRefusedUntilCompleted();
  testScannerLossDoesNotAbort();
  testFireSwitchFaultDuringCountdownAbortsAndNeverFires();
  testFireSwitchFaultDuringFiringEndsWindowImmediately();
  testRehearsalLightsLedBNotPins();
  testOneShotThenFreshStart();
  testIdleBeforeStart();
  testStartAfterCompletedClearsLatches();
  testStartAfterAbortClearsAborted();
  testAcceptsDisarmTrueDuringRehearsal();
  testAcceptsDisarmFalseDuringLiveFireOnly();
  testLiveInProgress();
  testLargeNowMsPastUint32Wrap();
  printf("fire sequence: OK\n");
}
