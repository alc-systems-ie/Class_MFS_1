#include "arming_sequence.hpp"

// No LOG_MODULE_REGISTER: the sequence is pure and reports through
// ArmingActions::SignalWarning() and TakeFailure().

namespace alc
{

  ArmingSequence::ArmingSequence(ArmingActions& actions)
      : m_actions(actions)
      , m_state(ArmState::Inactive)
      , m_deadline_ms(0)
      , m_failure_pending(false)
      , m_failure_step(ArmingStep::DisablePins)
      , m_failure_result(0)
  {}

  bool ArmingSequence::BeginArming(int64_t nowMs)
  {
    if (m_state != ArmState::Inactive) { return false; }

    // Nothing else: the fire pins are already disconnected while Inactive and
    // stay so for the whole exit delay.
    m_deadline_ms = nowMs + M_EXIT_DELAY_MS;
    m_state       = ArmState::Arming;
    return true;
  }

  bool ArmingSequence::Service(int64_t nowMs)
  {
    int result { 0 };

    if (m_state != ArmState::Arming) { return false; }
    if (nowMs < m_deadline_ms) { return false; }

    // One synchronous step. Nothing may tick the engine or derive the output
    // between the armed restart and the state becoming Active.
    result = m_actions.RestartDetection(true);
    if (result < 0) {
      failSafe();
      raiseFailure(ArmingStep::RestartDetection, result);
      return false;
    }

    // The LAST step. Active only once the pins are verified enabled.
    result = m_actions.EnableFirePins();
    if (result < 0) {
      failSafe();
      raiseFailure(ArmingStep::EnablePins, result);
      return false;
    }

    m_state = ArmState::Active;
    return true;
  }

  bool ArmingSequence::Disarm()
  {
    bool cancelled { m_state == ArmState::Arming };

    failSafe();
    return cancelled;
  }

  bool ArmingSequence::TakeFailure(ArmingStep& step, int& result)
  {
    if (!m_failure_pending) { return false; }

    step              = m_failure_step;
    result            = m_failure_result;
    m_failure_pending = false;
    return true;
  }

  void ArmingSequence::failSafe()
  {
    // DISARM ORDER - SAFETY CRITICAL. The pins go safe before any state
    // changes, and a disable failure does not stop the rest of the disarm.
    int disableResult { m_actions.DisableFirePins() };

    m_state = ArmState::Inactive;

    // The restart's result is deliberately not a warning: App logs it and the
    // engine retries the accelerometer on its own.
    (void)m_actions.RestartDetection(false);

    if (disableResult < 0) { raiseFailure(ArmingStep::DisablePins, disableResult); }
  }

  void ArmingSequence::raiseFailure(ArmingStep step, int result)
  {
    m_failure_pending = true;
    m_failure_step    = step;
    m_failure_result  = result;
    m_actions.SignalWarning(step, result);
  }

}
