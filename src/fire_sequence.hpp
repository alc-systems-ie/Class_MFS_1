#pragma once

#include <cstdint>

// Pure: no Zephyr headers, so the sequence compiles and is tested on the host.
// Time is passed in, like ArmingSequence (src/arming_sequence.hpp).
//
// Design: docs/superpowers/specs/2026-09-15-fire-command-amendment.md; plan
// Task 5, docs/superpowers/plans/2026-09-15-fire-command.md.

namespace alc
{

  /**
   * @brief The fire countdown's state. Idle before Start() and after an abort.
   */
  enum class FireState : uint8_t { Idle = 0, CountingDown = 1, Fired = 2 };

  /**
   * @brief A 10 s unstoppable countdown to a live fire or a rehearsal.
   *
   * `live` (set by Start()) decides only which latch a completed countdown
   * sets: FireLatched() for a live fire, consumed by App::updateOutputState()
   * to drive the fire pins; RehearsalFired() for a rehearsal, which lights LED
   * B only. NO SCANNER INPUT AT ALL - the scanner-loss exemption for a fire
   * countdown is proven by this class simply never calling out to one. The
   * only thing that stops the countdown once started is a fire-switch fault,
   * which aborts it outright and it never fires afterwards - App is expected
   * to fail safe (isolate pins, disarm, warn) on Aborted(). One-shot: once
   * Fired, further Service() calls do nothing and the state stays Fired -
   * there is no re-arm from inside this class. A fresh Start() begins an
   * entirely new countdown and clears every latch from the previous one.
   */
  class FireSequence
  {
    public:
      static constexpr uint32_t M_FIRE_COUNTDOWN_MS { 10000 };

      FireSequence();

      FireState State() const { return m_state; }

      /** @brief Idle (or Fired/aborted) -> CountingDown, deadline now + M_FIRE_COUNTDOWN_MS. */
      void Start(bool live, uint32_t nowMs);

      /**
       * @brief Advances the countdown. No-op unless CountingDown.
       *
       * `fireSwitchFaulty` aborts at once - Aborted() latches true, the state
       * returns to Idle, and the countdown never reaches Fired, whatever
       * nowMs becomes on a later call. Otherwise, at or after the deadline,
       * sets Fired and latches FireLatched() (live) or RehearsalFired()
       * (rehearsal).
       */
      void Service(uint32_t nowMs, bool fireSwitchFaulty);

      /** @brief One-shot: true once fired live. The only output App::updateOutputState() ORs in. */
      bool FireLatched() const { return m_fired; }

      /** @brief One-shot: true once fired in rehearsal - LED B only, never the fire pins. */
      bool RehearsalFired() const { return m_rehearsal_fired; }

      /** @brief True once a fire-switch fault aborted the countdown. */
      bool Aborted() const { return m_aborted; }

      /** @brief False while CountingDown - a Disarm is refused mid-countdown. */
      bool AcceptsDisarm() const { return m_state != FireState::CountingDown; }

    private:
      FireState m_state;
      bool m_live;
      uint32_t m_deadline_ms;
      bool m_fired;
      bool m_rehearsal_fired;
      bool m_aborted;
  };

}
