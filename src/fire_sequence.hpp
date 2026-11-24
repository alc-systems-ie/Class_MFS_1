#pragma once

#include <cstdint>

// Pure: no Zephyr headers, so the sequence compiles and is tested on the host.
// Time is passed in, like ArmingSequence (src/arming_sequence.hpp). Decoupled
// from DetectionEngine too: App passes assertMs rather than this header
// including detection_engine.hpp.
//
// Design: docs/superpowers/specs/2026-11-24-event-reports-design.md §5.2. FIRE
// no longer has a countdown; the fire gate (src/fire_gate.hpp) supplies the
// short announce delay before the output asserts.

namespace alc
{

  /**
   * @brief The fire window's state. Idle before Start() and after Cancel().
   */
  enum class FireState : uint8_t { Idle = 0, Firing = 1, Completed = 2 };

  /**
   * @brief A live fire or a rehearsal: a bounded assertion window that starts at once.
   *
   * `live` (set by Start()) decides which latch the Firing state sets -
   * FireLatched() for a live fire, consumed by App::updateOutputState() to
   * drive the fire pins; RehearsalFired() for a rehearsal, which lights the red
   * LED while disarmed only.
   *
   * There is no uninterruptible period: the device always fails safe. A fire
   * switch fault ends the window (Service()), and App cancels it with Cancel()
   * when a Disarm arrives during a live window.
   *
   * Start() asserts for the `assertMs` passed to it, then Service() drops to
   * Completed. This mirrors a motion trigger's output pulse -
   * DetectionEngine::NoteOutput completes a one-shot on the output's FALLING
   * edge, so a permanently latched fire would never let the device finish
   * latching Inactive. Once Completed, further Service() calls do nothing;
   * a fresh Start() begins an entirely new window and clears every latch.
   */
  class FireSequence
  {
    public:
      FireSequence();

      FireState State() const { return m_state; }

      /**
       * @brief Idle, Completed or Firing -> Firing at once, for `assertMs` from `nowMs`.
       */
      void Start(bool live, int64_t nowMs, int64_t assertMs);

      /**
       * @brief Advances the assertion window. No-op unless Firing.
       *
       * `fireSwitchFaulty` or the window elapsing ends the assertion at once
       * and moves to Completed.
       */
      void Service(int64_t nowMs, bool fireSwitchFaulty);

      /** @brief Firing -> Idle with nothing latched (a Disarm during a live window). No-op otherwise. */
      void Cancel();

      /** @brief True only during Firing, and only for a live fire - the only output App::updateOutputState() ORs in. */
      bool FireLatched() const { return m_state == FireState::Firing && m_live; }

      /** @brief True only during Firing, and only for a rehearsal - the red LED only, never the fire pins. */
      bool RehearsalFired() const { return m_state == FireState::Firing && !m_live; }

    private:
      FireState m_state;
      bool m_live;
      int64_t m_assert_ms;
      int64_t m_window_end_ms;
  };

}
