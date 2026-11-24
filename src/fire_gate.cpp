#include "fire_gate.hpp"

namespace alc
{

  bool FireGate::Update(bool fireCondition, int64_t nowMs)
  {
    if (!fireCondition) {
      m_active       = false;
      m_announce_due = false;
      m_skipped      = false;
      return false;
    }

    if (!m_active) {
      m_active       = true;
      m_announce_due = true;
      m_skipped      = false;
      m_start_ms     = nowMs;
      return false;
    }

    // A clock that reads before the start is treated as not yet elapsed.
    return m_skipped || (nowMs >= m_start_ms && nowMs - m_start_ms >= M_ANNOUNCE_MS);
  }

  bool FireGate::TakeAnnounceDue()
  {
    bool due { m_announce_due };

    m_announce_due = false;
    return due;
  }

  void FireGate::Skip()
  {
    if (m_active) { m_skipped = true; }
  }

}
