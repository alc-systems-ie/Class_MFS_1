#pragma once

#include <cstdint>

namespace alc
{

  /**
   * @brief The acknowledgement patterns LED A plays. PROVISIONAL scheme.
   *
   * A pattern plays only after an AUTHENTICATED command has been accepted and
   * only once the new state is real. A failed authentication never shows
   * anything, or the LED would tell an attacker which guesses got through.
   * See the design spec section 6.7.
   */
  enum class LedPattern : uint8_t {
    None,
    Armed,                  ///< Rapid flash ~8 Hz, 3 s.
    Disarmed,               ///< Slow flash 1 Hz, 3 s.
    DisarmedDelayCancelled, ///< Double blink each second, 3 s - a pending trigger really was cancelled.
    ArmRefused,             ///< Three long pulses, 3 s - the accelerometer would not configure.
    SettingsApplied,        ///< One 200 ms blink.
    ModeChanged,            ///< Two 200 ms blinks - slot-0 command changed the operating mode.
  };

  /**
   * @brief Time-based pattern player. Pure: the caller supplies the time.
   *
   * Queried from a 10 ms timer while a pattern is active, so the 60 ms phases of
   * the Armed pattern are rendered faithfully despite the 100 ms main loop.
   */
  class LedSequencer
  {
    public:
      LedSequencer();

      /** @brief Start a pattern, replacing any pattern already playing. */
      void Start(LedPattern pattern, int64_t nowMs);

      void Stop() { m_pattern = LedPattern::None; }

      LedPattern Current() const { return m_pattern; }

      /** @brief True until the pattern's duration has elapsed. */
      bool IsActive(int64_t nowMs) const;

      /** @brief The LED level the pattern calls for at this instant. False once inactive. */
      bool Level(int64_t nowMs) const;

    private:
      LedPattern m_pattern;
      int64_t m_start_ms;
  };

}
