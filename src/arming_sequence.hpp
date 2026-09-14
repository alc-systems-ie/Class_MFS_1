#pragma once

#include <cerrno>
#include <cstdint>

// Pure: no Zephyr headers, so the sequence compiles and is tested on the host.
// Time is passed in; every hardware effect goes through ArmingActions.
//
// ArmState lives here rather than in arm_policy.hpp or App: this header needs
// only <cstdint>, so arm_policy.hpp can include it without acquiring anything
// heavier, and the sequence that owns the state defines it.
//
// Design: docs/superpowers/specs/2026-09-14-arming-sequence-amendment.md.

namespace alc
{

  /**
   * @brief The device's arm state. Cold start is Inactive.
   *
   * Arming sits between Inactive and Active: an Arm command was accepted and the
   * exit delay is running. The fire pins stay disconnected until Active.
   */
  enum class ArmState : uint8_t { Inactive = 0, Arming = 1, Active = 2 };

  /**
   * @brief The step a failure or warning belongs to.
   *
   * ScannerCheck: the scanner was not running when an Arm was accepted, or was not
   * running - or had gone down since last checked - at the end of the exit delay or
   * just before the fire pins were enabled. ScannerLost: it went down while Arming
   * or Active and ServiceScannerHealth() failed safe.
   */
  enum class ArmingStep : uint8_t { DisablePins, RestartDetection, EnablePins, ScannerCheck, ScannerLost };

  /**
   * @brief The hardware effects the arming sequence drives. Implemented by App.
   *
   * RE-ENTRANCY: RestartDetection(true) and EnableFirePins() may re-enter
   * ArmingSequence::Disarm() (a disarm command handled while they run), and may
   * follow it with BeginArming(). The disarm wins - see ArmingSequence::Service().
   * DisableFirePins(), RestartDetection(false) and SignalWarning() must not call
   * back into the sequence: they run inside the fail-safe itself.
   */
  class ArmingActions
  {
    public:
      virtual ~ArmingActions() = default;

      /** @brief Drive both fire pins low, then disconnect both. 0 or negative errno. */
      virtual int DisableFirePins() = 0;

      /** @brief Restart the detection engine from zero - DetectionEngine::Restart via App. */
      virtual int RestartDetection(bool armed) = 0;

      /** @brief Configure the fire pins as outputs, inactive, verified low. 0 or negative errno. */
      virtual int EnableFirePins() = 0;

      /**
       * @brief Whether the command scanner is running, so a Disarm could be heard.
       *
       * A query only: must not call back into ArmingSequence.
       */
      virtual bool ScannerRunning() const = 0;

      /**
       * @brief Whether the scanner has gone down since the last call. Clears the latch.
       *
       * LATCHED, so a loss that heals before the sequence next looks - a scanner
       * call that ended with the scanner stopped, followed by a successful retry -
       * is still seen. Set whenever a scanner operation ends with it not running,
       * in any arm state. A query of App state: must not call back into
       * ArmingSequence.
       */
      virtual bool TakeScannerLost() = 0;

      /**
       * @brief Raise the warning for a failed step (amendment section 4).
       *
       * Must not call back into ArmingSequence.
       */
      virtual void SignalWarning(ArmingStep step, int result) = 0;
  };

  /**
   * @brief Inactive -> Arming -> Active, and every way back to Inactive.
   *
   * SAFETY CRITICAL ORDERING (amendment sections 2 and 3):
   *
   * - Arm: Arming for exactly M_EXIT_DELAY_MS with nothing done, then in one
   *   synchronous Service() call: RestartDetection(true) -> EnableFirePins() ->
   *   Active. Active is set only after the enable returned 0; the enable is never
   *   attempted if the restart failed.
   * - Any arming-step failure: DisableFirePins() -> Inactive ->
   *   RestartDetection(false) -> SignalWarning(step, result).
   * - Scanner lost while Arming or Active (ServiceScannerHealth()): the same
   *   fail-safe order, with step ScannerLost - including a loss that has healed
   *   by the time it is checked (ArmingActions::TakeScannerLost()).
   * - Disarm, from ANY state: DisableFirePins() FIRST -> Inactive ->
   *   RestartDetection(false). A disable failure does not stop the disarm; it is
   *   warned after the disarm completes.
   *
   * RE-ENTRANCY: if a callback re-enters Disarm() during Service(), the disarm
   * wins. Service() checks after each callback whether its arming is still the
   * current one (state Arming and the same arming session); if not it returns
   * false at once, never enables or goes Active and raises no warning. A disarm
   * inside EnableFirePins() is followed by one more DisableFirePins(), because
   * the enable may have configured the pins after the nested disable.
   *
   * The sequence has no output-derivation hook: the disarm order's "re-derive
   * the output" step (between Inactive and the restart) belongs at the start of
   * App's RestartDetection(false), before the engine is restarted.
   */
  class ArmingSequence
  {
    public:
      static constexpr int64_t M_EXIT_DELAY_MS { 10000 };

      explicit ArmingSequence(ArmingActions& actions);

      ArmState State() const { return m_state; }

      /**
       * @brief Inactive -> Arming, deadline now + M_EXIT_DELAY_MS. Ignored (returns false) unless Inactive.
       *
       * Refused (returns false, still Inactive, nothing started) when the scanner
       * is not running: SignalWarning(ScannerCheck, -ENODEV) and recorded for
       * TakeFailure(). A latched scanner loss is discarded first: it happened
       * while Inactive, and only whether the scanner is running now matters.
       */
      bool BeginArming(int64_t nowMs);

      /**
       * @brief Services the exit delay. No-op returning false unless Arming.
       *
       * Does nothing before the deadline. At or after it checks the scanner is
       * running and has not gone down since last checked (else fails safe, step
       * ScannerCheck), then runs restart -> the same scanner check again (the
       * restart touches the scanner cadence) -> enable -> Active. Returns true on
       * the call that went Active, false otherwise
       * (including a failed arming, which is reported through SignalWarning()
       * and TakeFailure()).
       */
      bool Service(int64_t nowMs);

      /**
       * @brief ALWAYS FAIL SAFE on scanner loss (owner rule 2026-09-14). Call every main-loop tick.
       *
       * Consumes the scanner-loss latch on every call, in every state, so a loss
       * while Inactive cannot fail safe a later arming. No-op returning false when
       * Inactive, or when the scanner is running and has not gone down since the
       * last call. While Arming or Active with the scanner not running - or having
       * gone down at any point since, even if running again now, because a Disarm
       * may have been missed while it was down - fails safe at once through the
       * disarm order: DisableFirePins() ->
       * Inactive (arming cancelled, any pending trigger cancelled by the disarmed
       * restart) -> RestartDetection(false) -> SignalWarning(ScannerLost, -ENODEV),
       * also recorded for TakeFailure(). Returns true on that call only; once
       * Inactive it raises nothing more, so the warning is once per event.
       *
       * Calls only TakeScannerLost(), ScannerRunning() and the fail-safe callbacks,
       * none of which may re-enter the sequence, so no session check follows.
       */
      bool ServiceScannerHealth();

      /**
       * @brief From any state: DisableFirePins() FIRST, then Inactive, then RestartDetection(false).
       *
       * The restart's own result is not a warning (amendment section 4); the
       * implementation of RestartDetection() logs and retries it.
       *
       * @return Whether arming was cancelled - true iff the state was Arming.
       */
      bool Disarm();

      /**
       * @brief The last failure, if any since the last read. Cleared by the read.
       *
       * Records arming-step failures (ScannerCheck, RestartDetection, EnablePins), a
       * ScannerLost fail-safe and a DisablePins failure, whether in Disarm() or in an arming fail-safe. A
       * DisablePins failure is sticky: a later arming-step failure does not
       * overwrite it before it is read, because the pins may not be isolated.
       *
       * @return false, outputs untouched, when there is none.
       */
      bool TakeFailure(ArmingStep& step, int& result);

    private:
      // The result reported with a ScannerCheck or ScannerLost failure.
      static constexpr int M_SCANNER_NOT_RUNNING { -ENODEV };

      // Consumes the scanner-loss latch, then reports whether the scanner is down
      // now or went down since the latch was last consumed.
      bool scannerDownOrLost();

      // DisableFirePins() -> Inactive -> RestartDetection(false). A disable
      // failure is warned and recorded here, after the restart.
      void failSafe();

      // Whether the arming Service() captured as `session` has been cancelled or
      // replaced by a callback re-entering Disarm() (and perhaps BeginArming()).
      bool superseded(uint32_t session) const;

      void raiseFailure(ArmingStep step, int result);

      ArmingActions& m_actions;
      ArmState m_state;
      int64_t m_deadline_ms;
      uint32_t m_session;
      bool m_failure_pending;
      ArmingStep m_failure_step;
      int m_failure_result;
  };

}
