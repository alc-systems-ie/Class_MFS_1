#pragma once

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

  /** @brief The step a failure or warning belongs to. */
  enum class ArmingStep : uint8_t { DisablePins, RestartDetection, EnablePins };

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

      /** @brief Inactive -> Arming, deadline now + M_EXIT_DELAY_MS. Ignored (returns false) unless Inactive. */
      bool BeginArming(int64_t nowMs);

      /**
       * @brief Services the exit delay. No-op returning false unless Arming.
       *
       * Does nothing before the deadline. At or after it runs restart -> enable ->
       * Active. Returns true on the call that went Active, false otherwise
       * (including a failed arming, which is reported through SignalWarning()
       * and TakeFailure()).
       */
      bool Service(int64_t nowMs);

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
       * Records arming-step failures (RestartDetection, EnablePins) and a
       * DisablePins failure, whether in Disarm() or in an arming fail-safe. A
       * DisablePins failure is sticky: a later arming-step failure does not
       * overwrite it before it is read, because the pins may not be isolated.
       *
       * @return false, outputs untouched, when there is none.
       */
      bool TakeFailure(ArmingStep& step, int& result);

    private:
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
