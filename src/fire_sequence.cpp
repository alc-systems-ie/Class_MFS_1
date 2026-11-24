#include "fire_sequence.hpp"

// No LOG_MODULE_REGISTER: the sequence is pure and reports only through its
// own query methods (FireLatched(), RehearsalFired()) - App logs.

namespace alc
{

  FireSequence::FireSequence()
      : m_state(FireState::Idle)
      , m_live(false)
      , m_assert_ms(0)
      , m_window_end_ms(0)
  {}

  void FireSequence::Start(bool live, int64_t nowMs, int64_t assertMs)
  {
    // A fresh window: every latch from a previous Start() is cleared, so a
    // rehearsal fired twice in a row is seen as two distinct events.
    m_state         = FireState::Firing;
    m_live          = live;
    m_assert_ms     = assertMs;
    m_window_end_ms = nowMs + assertMs;
  }

  void FireSequence::Service(int64_t nowMs, bool fireSwitchFaulty)
  {
    if (m_state != FireState::Firing) { return; }

    // A fault while asserting ends the window at once.
    if (fireSwitchFaulty || nowMs >= m_window_end_ms) { m_state = FireState::Completed; }
  }

  void FireSequence::Cancel()
  {
    if (m_state == FireState::Firing) { m_state = FireState::Idle; }
  }

}
