#include "fire_sequence.hpp"

// No LOG_MODULE_REGISTER: the sequence is pure and reports only through its
// own query methods (Aborted(), FireLatched(), RehearsalFired()) - App logs.

namespace alc
{

  FireSequence::FireSequence()
      : m_state(FireState::Idle)
      , m_live(false)
      , m_deadline_ms(0)
      , m_fired(false)
      , m_rehearsal_fired(false)
      , m_aborted(false)
  {}

  void FireSequence::Start(bool live, uint32_t nowMs)
  {
    // A fresh countdown: every latch from a previous Start() is cleared, so a
    // rehearsal fired twice in a row is seen as two distinct events.
    m_state           = FireState::CountingDown;
    m_live            = live;
    m_deadline_ms     = nowMs + M_FIRE_COUNTDOWN_MS;
    m_fired           = false;
    m_rehearsal_fired = false;
    m_aborted         = false;
  }

  void FireSequence::Service(uint32_t nowMs, bool fireSwitchFaulty)
  {
    if (m_state != FireState::CountingDown) { return; }

    // UNSTOPPABLE except by a fire-switch fault - no scanner input at all, so
    // a lost scanner cannot abort a countdown already running.
    if (fireSwitchFaulty) {
      m_state   = FireState::Idle;
      m_aborted = true;
      return;
    }

    if (nowMs < m_deadline_ms) { return; }

    m_state = FireState::Fired;
    if (m_live) {
      m_fired = true;
    } else {
      m_rehearsal_fired = true;
    }
  }

}
