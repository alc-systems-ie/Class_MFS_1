#include <cassert>
#include <cstdio>

#include "fire_sequence.hpp"

// Host tests for FireSequence. Event reports: FIRE has no countdown, so Start()
// goes straight to the bounded Firing assertion window.

namespace
{

  using namespace alc;

  constexpr int64_t M_START_MS { 0 };
  constexpr int64_t M_ONE_MS { 1 };
  constexpr int64_t M_ASSERT_MS { 500 };
  constexpr int64_t M_LARGE_START_MS { 5'000'000'000 };
  constexpr int64_t M_MID_WINDOW_MS { 100 };

  // A live Start() latches FireLatched() at once, drops at the end of the window and completes.
  void testLiveFiresAtOnceThenDrops()
  {
    FireSequence sequence;

    assert(sequence.State() == FireState::Idle);
    sequence.Start(/*live=*/true, M_START_MS, M_ASSERT_MS);
    sequence.Service(M_START_MS, false);
    assert(sequence.State() == FireState::Firing);
    assert(sequence.FireLatched());
    assert(!sequence.RehearsalFired());

    sequence.Service(M_START_MS + M_ASSERT_MS - M_ONE_MS, false);
    assert(sequence.FireLatched());

    sequence.Service(M_START_MS + M_ASSERT_MS, false);
    assert(!sequence.FireLatched());
    assert(sequence.State() == FireState::Completed);

    // One-shot: later calls change nothing.
    sequence.Service(M_START_MS + M_ASSERT_MS * 2, false);
    assert(sequence.State() == FireState::Completed);
  }

  // The state is Firing immediately after Start(), before any Service().
  void testStartGoesStraightToFiring()
  {
    FireSequence sequence;

    sequence.Start(true, M_START_MS, M_ASSERT_MS);
    assert(sequence.State() == FireState::Firing);
    assert(sequence.FireLatched());
  }

  // A rehearsal sets only RehearsalFired().
  void testRehearsalSetsOnlyRehearsal()
  {
    FireSequence sequence;

    sequence.Start(/*live=*/false, M_START_MS, M_ASSERT_MS);
    sequence.Service(M_START_MS, false);
    assert(sequence.RehearsalFired());
    assert(!sequence.FireLatched());

    sequence.Service(M_START_MS + M_ASSERT_MS, false);
    assert(!sequence.RehearsalFired());
    assert(sequence.State() == FireState::Completed);
  }

  // A fire-switch fault while Firing completes the window at once.
  void testFaultWhileFiringCompletes()
  {
    FireSequence sequence;

    sequence.Start(true, M_START_MS, M_ASSERT_MS);
    sequence.Service(M_START_MS + M_MID_WINDOW_MS, /*fireSwitchFaulty=*/true);
    assert(!sequence.FireLatched());
    assert(sequence.State() == FireState::Completed);
  }

  // Cancel() while Firing returns to Idle with nothing latched.
  void testCancelWhileFiringGoesIdle()
  {
    FireSequence sequence;

    sequence.Start(true, M_START_MS, M_ASSERT_MS);
    sequence.Cancel();
    assert(sequence.State() == FireState::Idle);
    assert(!sequence.FireLatched());
    assert(!sequence.RehearsalFired());

    sequence.Service(M_START_MS + M_MID_WINDOW_MS, false);
    assert(sequence.State() == FireState::Idle);
  }

  // Cancel() when idle is harmless.
  void testCancelWhenIdle()
  {
    FireSequence sequence;

    sequence.Cancel();
    assert(sequence.State() == FireState::Idle);
  }

  // A fresh Start() after Completed or Cancel() works, and a rehearsal then a live fire do not mix latches.
  void testRestartAfterCompletedAndCancel()
  {
    FireSequence sequence;

    sequence.Start(false, M_START_MS, M_ASSERT_MS);
    sequence.Service(M_START_MS + M_ASSERT_MS, false);
    assert(sequence.State() == FireState::Completed);

    sequence.Start(true, M_LARGE_START_MS, M_ASSERT_MS);
    assert(sequence.FireLatched());
    assert(!sequence.RehearsalFired());
    sequence.Service(M_LARGE_START_MS + M_ASSERT_MS - M_ONE_MS, false);
    assert(sequence.FireLatched());
    sequence.Service(M_LARGE_START_MS + M_ASSERT_MS, false);
    assert(sequence.State() == FireState::Completed);

    sequence.Start(true, M_LARGE_START_MS, M_ASSERT_MS);
    sequence.Cancel();
    sequence.Start(true, M_LARGE_START_MS, M_ASSERT_MS);
    assert(sequence.FireLatched());
  }

}

void run_fire_sequence_tests()
{
  testLiveFiresAtOnceThenDrops();
  testStartGoesStraightToFiring();
  testRehearsalSetsOnlyRehearsal();
  testFaultWhileFiringCompletes();
  testCancelWhileFiringGoesIdle();
  testCancelWhenIdle();
  testRestartAfterCompletedAndCancel();
  std::printf("fire_sequence tests passed\n");
}
