#include "fire_sequence.hpp"

// No LOG_MODULE_REGISTER: the sequence is pure and reports only through its
// own query methods (Aborted(), FireLatched(), RehearsalFired()) - App logs.

namespace alc
{

  FireSequence::FireSequence()
      : m_state(FireState::Idle)
      , m_live(false)
      , m_deadline_ms(0)
      , m_assert_ms(0)
      , m_window_end_ms(0)
      , m_aborted(false)
  {}

  void FireSequence::Start(bool live, int64_t nowMs, int64_t assertMs)
  {
    // A fresh countdown: every latch from a previous Start() is cleared, so a
    // rehearsal fired twice in a row is seen as two distinct events.
    m_state       = FireState::CountingDown;
    m_live        = live;
    m_deadline_ms = nowMs + M_FIRE_COUNTDOWN_MS;
    m_assert_ms   = assertMs;
    m_aborted     = false;
  }

  void FireSequence::Service(int64_t nowMs, bool fireSwitchFaulty)
  {
    if (m_state == FireState::CountingDown) {
      // UNSTOPPABLE except by a fire-switch fault - no scanner input at all,
      // so a lost scanner cannot abort a countdown already running.
      if (fireSwitchFaulty) {
        m_state   = FireState::Idle;
        m_aborted = true;
        return;
      }

      if (nowMs < m_deadline_ms) { return; }

      m_state         = FireState::Firing;
      m_window_end_ms = nowMs + m_assert_ms;
      return;
    }

    if (m_state == FireState::Firing) {
      // A fault while asserting ends the window at once, dropping the output
      // just as a fire-switch fault would while counting down.
      if (fireSwitchFaulty || nowMs >= m_window_end_ms) { m_state = FireState::Completed; }
      return;
    }
  }

}
