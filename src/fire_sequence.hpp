#pragma once

#include <cstdint>

// Pure: no Zephyr headers, so the sequence compiles and is tested on the host.
// Time is passed in, like ArmingSequence (src/arming_sequence.hpp). Decoupled
// from DetectionEngine too: App passes assertMs rather than this header
// including detection_engine.hpp.
//
// Design: docs/superpowers/specs/2026-09-15-fire-command-amendment.md; plan
// Task 5, docs/superpowers/plans/2026-09-15-fire-command.md; Task 5b amends
// the time type to int64_t and adds the bounded Firing assertion window.

namespace alc
{

  /**
   * @brief The fire countdown's state. Idle before Start() and after an abort.
   */
  enum class FireState : uint8_t { Idle = 0, CountingDown = 1, Firing = 2, Completed = 3 };

  /**
   * @brief A 10 s unstoppable countdown to a live fire or a rehearsal, then a bounded assertion window.
   *
   * `live` (set by Start()) decides which latch the Firing state sets -
   * FireLatched() for a live fire, consumed by App::updateOutputState() to
   * drive the fire pins; RehearsalFired() for a rehearsal, which lights LED B
   * only - and also gates LiveInProgress()/AcceptsDisarm(): amendment §4's
   * uninterruptible-by-Disarm carve-out applies to a LIVE fire only, so a
   * rehearsal countdown or its assertion window remains interruptible.
   * NO SCANNER INPUT AT ALL - the scanner-loss exemption for a fire countdown
   * is proven by this class simply never calling out to one; App additionally
   * scopes that exemption to LiveInProgress() so an overlapping rehearsal does
   * not defer an Arming device's scanner fail-safe. The only thing that stops
   * the countdown once started is a fire-switch fault, which aborts it
   * outright before it ever fires - App is expected to fail safe (isolate
   * pins, disarm, warn) on Aborted().
   *
   * The countdown expiring does not latch forever: it enters Firing, asserts
   * for the `assertMs` passed to Start(), then drops to Completed. This
   * mirrors a motion trigger's output pulse - DetectionEngine::NoteOutput
   * completes a one-shot on the output's FALLING edge, so a permanently
   * latched fire would never let the device finish latching Inactive.
   * One-shot: once Completed, further Service() calls do nothing and the
   * state stays Completed - there is no re-arm from inside this class. A
   * fresh Start() begins an entirely new countdown and clears every latch
   * from the previous one.
   */
  class FireSequence
  {
    public:
      static constexpr int64_t M_FIRE_COUNTDOWN_MS { 10000 };

      FireSequence();

      FireState State() const { return m_state; }

      /**
       * @brief Idle (or Completed/aborted) -> CountingDown, deadline now + M_FIRE_COUNTDOWN_MS.
       *
       * `assertMs` is how long the fire output asserts once the countdown
       * expires (App passes DetectionEngine::M_DELAYED_TRIGGER_HOLD_MS) -
       * captured here and used when the countdown expires in Service().
       */
      void Start(bool live, int64_t nowMs, int64_t assertMs);

      /**
       * @brief Advances the countdown and the assertion window. No-op unless CountingDown or Firing.
       *
       * `fireSwitchFaulty` while CountingDown aborts at once - Aborted()
       * latches true, the state returns to Idle, and the countdown never
       * reaches Firing, whatever nowMs becomes on a later call. Otherwise, at
       * or after the deadline, enters Firing and latches FireLatched() (live)
       * or RehearsalFired() (rehearsal) for the assertion window captured at
       * Start(). While Firing, `fireSwitchFaulty` or the window elapsing ends
       * the assertion at once and moves to Completed.
       */
      void Service(int64_t nowMs, bool fireSwitchFaulty);

      /** @brief True only during Firing, and only for a live fire - the only output App::updateOutputState() ORs in. */
      bool FireLatched() const { return m_state == FireState::Firing && m_live; }

      /** @brief True only during Firing, and only for a rehearsal - LED B only, never the fire pins. */
      bool RehearsalFired() const { return m_state == FireState::Firing && !m_live; }

      /** @brief True once a fire-switch fault aborted the countdown before it fired. */
      bool Aborted() const { return m_aborted; }

      /** @brief True only while a LIVE fire is CountingDown or Firing - false for a rehearsal in either state, and false when Idle/Completed. */
      bool LiveInProgress() const { return m_live && (m_state == FireState::CountingDown || m_state == FireState::Firing); }

      /**
       * @brief False only while a LIVE fire is CountingDown or Firing.
       *
       * Uninterruptible by a Disarm command applies to a LIVE fire event only
       * (amendment §4: "once FIRE is accepted while Active"). A rehearsal
       * countdown or its assertion window is interruptible - AcceptsDisarm()
       * is true throughout.
       */
      bool AcceptsDisarm() const { return !LiveInProgress(); }

    private:
      FireState m_state;
      bool m_live;
      int64_t m_deadline_ms;
      int64_t m_assert_ms;
      int64_t m_window_end_ms;
      bool m_aborted;
  };

}
