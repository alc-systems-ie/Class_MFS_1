#include "arming_sequence.hpp"

// No LOG_MODULE_REGISTER: the sequence is pure and reports through
// ArmingActions::SignalWarning() and TakeFailure().

namespace alc
{

  ArmingSequence::ArmingSequence(ArmingActions& actions)
      : m_actions(actions)
      , m_state(ArmState::Inactive)
      , m_deadline_ms(0)
      , m_session(0)
      , m_failure_pending(false)
      , m_failure_step(ArmingStep::DisablePins)
      , m_failure_result(0)
  {}

  bool ArmingSequence::BeginArming(int64_t nowMs)
  {
    if (m_state != ArmState::Inactive) { return false; }

    // A latched loss belongs to the Inactive period before this Arm, when there
    // was nothing to fail safe. Discarded so it cannot disarm the new arming;
    // whether the scanner is running NOW is what the check below decides.
    (void)m_actions.TakeScannerLost();

    // NO SCANNER, NO ARMING - SAFETY CRITICAL. Commands arrive only by scanning,
    // so an armed device that cannot hear a Disarm could leave the armed state
    // only by triggering. Refused outright: still Inactive, no delay, no pin or
    // restart action - only the warning.
    if (!m_actions.ScannerRunning()) {
      raiseFailure(ArmingStep::ScannerCheck, M_SCANNER_NOT_RUNNING);
      return false;
    }

    // Nothing else: the fire pins are already disconnected while Inactive and
    // stay so for the whole exit delay.
    m_deadline_ms = nowMs + M_EXIT_DELAY_MS;
    m_state       = ArmState::Arming;
    m_session++;
    return true;
  }

  bool ArmingSequence::Service(int64_t nowMs)
  {
    if (m_state != ArmState::Arming) { return false; }
    if (nowMs < m_deadline_ms) { return false; }

    int result { 0 };
    int disableResult { 0 };
    uint32_t session { m_session };

    // NO SCANNER, NO ARMING. The scanner may have stopped during the exit delay -
    // and perhaps been restarted by a retry since the last health check, so the
    // latch counts too. Checked before the armed restart, so nothing is restarted
    // or enabled. Both calls are queries and cannot re-enter, so no session check
    // follows.
    if (scannerDownOrLost()) {
      failSafe();
      raiseFailure(ArmingStep::ScannerCheck, M_SCANNER_NOT_RUNNING);
      return false;
    }

    // One synchronous step. Nothing may tick the engine or derive the output
    // between the armed restart and the state becoming Active.
    result = m_actions.RestartDetection(true);

    // RE-ENTRANCY. A Disarm() handled inside the restart already made
    // everything safe and wins outright: no enable, no Active, and no warning,
    // because the disarm was the engineer's rather than a failure. Checked
    // before the result, and by session, so a disarm followed by a fresh arm
    // is not mistaken for this arming.
    if (superseded(session)) { return false; }

    if (result < 0) {
      failSafe();
      raiseFailure(ArmingStep::RestartDetection, result);
      return false;
    }

    // The armed restart cancels any delay, which sets the scanner cadence and can
    // itself take the scanner down. Checked again before the pins are enabled, so
    // a device that has just gone deaf is never armed, even for one tick.
    if (scannerDownOrLost()) {
      failSafe();
      raiseFailure(ArmingStep::ScannerCheck, M_SCANNER_NOT_RUNNING);
      return false;
    }

    // The LAST step. Active only once the pins are verified enabled.
    result = m_actions.EnableFirePins();

    // RE-ENTRANCY. A Disarm() handled inside the enable disabled the pins, but
    // the enable may have configured them again after it. Disable them once
    // more, whatever the enable returned, so a disable is the last pin action.
    if (superseded(session)) {
      disableResult = m_actions.DisableFirePins();
      if (disableResult < 0) { raiseFailure(ArmingStep::DisablePins, disableResult); }
      return false;
    }

    if (result < 0) {
      failSafe();
      raiseFailure(ArmingStep::EnablePins, result);
      return false;
    }

    m_state = ArmState::Active;
    return true;
  }

  bool ArmingSequence::ServiceScannerHealth()
  {
    // Consumed in EVERY state, before the Inactive guard, so a loss while
    // Inactive is spent here and cannot fail safe a later arming.
    bool scannerDown { scannerDownOrLost() };

    if (m_state == ArmState::Inactive) { return false; }
    if (!scannerDown) { return false; }

    // ALWAYS FAIL SAFE (owner rule 2026-09-14). Commands arrive only by scanning,
    // so a device Arming or Active that cannot hear a Disarm must not keep any
    // path to firing. A loss that has already healed counts: a Disarm may have
    // been sent while the scanner was down, and a pending trigger may already
    // have been suppressed because of it. The ordinary disarm order - pins
    // first - then the warning.
    failSafe();
    raiseFailure(ArmingStep::ScannerLost, M_SCANNER_NOT_RUNNING);
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
    m_session++;

    // The restart's result is deliberately not a warning: App logs it and the
    // engine retries the accelerometer on its own.
    (void)m_actions.RestartDetection(false);

    if (disableResult < 0) { raiseFailure(ArmingStep::DisablePins, disableResult); }
  }

  bool ArmingSequence::scannerDownOrLost()
  {
    // Latch first, so it is consumed whatever the running check says.
    bool lost { m_actions.TakeScannerLost() };

    return lost || !m_actions.ScannerRunning();
  }

  bool ArmingSequence::superseded(uint32_t session) const
  {
    return (m_state != ArmState::Arming) || (m_session != session);
  }

  void ArmingSequence::raiseFailure(ArmingStep step, int result)
  {
    // STICKY. A pin disable failure means the pins may not be isolated, which
    // outranks any arming-step failure raised with it, so it is not overwritten
    // until TakeFailure() reads it. Every failure is still warned.
    bool keepDisableFailure { m_failure_pending && (m_failure_step == ArmingStep::DisablePins) && (step != ArmingStep::DisablePins) };

    if (!keepDisableFailure) {
      m_failure_pending = true;
      m_failure_step    = step;
      m_failure_result  = result;
    }
    m_actions.SignalWarning(step, result);
  }

}
