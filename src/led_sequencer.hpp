#pragma once

#include <cstdint>

namespace alc
{

  /**
   * @brief The acknowledgement patterns LED A plays, and the interim warning on LED B. PROVISIONAL scheme.
   *
   * An LED A pattern plays only after an AUTHENTICATED command has been accepted
   * and only once the new state is real. A failed authentication never shows
   * anything, or the LED would tell an attacker which guesses got through.
   * See the design spec section 6.7.
   *
   * Warning is not an acknowledgement: it plays on LED B, from App::signalWarning(),
   * until the dedicated warning light is chosen (arming sequence amendment section 4).
   */
  enum class LedPattern : uint8_t {
    None,
    Armed,                  ///< Rapid flash ~8 Hz, 3 s.
    Disarmed,               ///< Slow flash 1 Hz, 3 s.
    DisarmedDelayCancelled, ///< Double blink each second, 3 s - a pending trigger really was cancelled.
    Warning,                ///< Three long pulses, 3 s, on LED B - the interim warning (arming failure, fire switch fault).
    SettingsApplied,        ///< One 200 ms blink.
    ModeChanged,            ///< Two 200 ms blinks - slot-0 command changed the operating mode.
  };

  /**
   * @brief Time-based pattern player. Pure: the caller supplies the time.
   *
   * Queried from a 10 ms timer while a pattern is active, so the 60 ms phases of
   * the Armed pattern are rendered faithfully despite the 100 ms main loop. App
   * holds one per LED: LED A's acknowledgements and LED B's warning.
   */
  class LedSequencer
  {
    public:
      // Dark gap played before and after every pattern. A bench build lights
      // LED A steadily while Inactive, and a pattern that begins or ends on an
      // on phase would merge into that level - a single 200 ms blink on a lit
      // LED is simply invisible (found on the bench 2026-09-14). Framed in dark,
      // every pattern reads the same on a lit or a dark idle LED.
      static constexpr uint16_t M_FRAME_GAP_MS { 300 };

      LedSequencer();

      /** @brief Start a pattern, replacing any pattern already playing. */
      void Start(LedPattern pattern, int64_t nowMs);

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
