#pragma once

#include <cstdint>

namespace alc
{

  /**
   * @brief The overlay patterns, each on one LED. See
   *        docs/superpowers/specs/2026-11-12-three-led-indication-design.md section 3.2.
   *
   * A confirmation plays only after an AUTHENTICATED command has been accepted
   * and only once the new state is real. A failed authentication never shows
   * anything, or the LED would tell an attacker which guesses got through.
   */
  enum class LedPattern : uint8_t {
    None,
    RedFlash,     ///< Rapid flash ~8 Hz, 3 s, red - arming completed, or an armed device replaying its state.
    OrangeTriple, ///< Three 150 ms flashes in 1 s, orange - an accepted command or time set confirmed.
    OrangeSteady, ///< On for 3 s, orange - arming refused, or any fault (App::signalWarning()).
  };

  /** @brief The three LEDs, by colour. */
  enum class LedColour : uint8_t {
    None,
    Red,    ///< LED1, P2.02.
    Orange, ///< LED2, P2.03.
    Green,  ///< LED3, P2.04.
  };

  /** @brief The LED a pattern plays on. LedColour::None for LedPattern::None. */
  LedColour ColourOf(LedPattern pattern);

  /**
   * @brief Time-based pattern player. Pure: the caller supplies the time.
   *
   * Queried from a 10 ms timer while a pattern is active, so the 60 ms phases of
   * the red flash are rendered faithfully despite the 100 ms main loop. App holds
   * one: only one overlay plays at a time, chosen by priority - see Start().
   */
  class LedSequencer
  {
    public:
      // Dark gap played before and after every pattern. A pattern that begins or
      // ends on an on phase would merge into a lit base level - a single 200 ms
      // blink on a lit LED is simply invisible (found on the bench 2026-09-14).
      // Framed in dark, every pattern reads the same on a lit or a dark LED.
      static constexpr uint16_t M_FRAME_GAP_MS { 300 };

      LedSequencer();

      /**
       * @brief Start a pattern, replacing the one playing unless that one outranks it.
       *
       * Priority, highest first: OrangeSteady, RedFlash, OrangeTriple. A pattern of
       * equal or higher priority replaces the one playing and restarts from its
       * lead-in; a lower one is refused while the current pattern is still active,
       * so a confirmation never cuts short a warning (spec section 3.3).
       *
       * @return True if the pattern started; false if it was refused.
       */
      bool Start(LedPattern pattern, int64_t nowMs);

      void Stop() { m_pattern = LedPattern::None; }

      LedPattern Current() const { return m_pattern; }

      /** @brief True until the pattern and both of its dark gaps have elapsed. */
      bool IsActive(int64_t nowMs) const;

      /** @brief The LED level the pattern calls for at this instant. False once inactive. */
      bool Level(int64_t nowMs) const;

    private:
      LedPattern m_pattern;
      int64_t m_start_ms;
  };

}
