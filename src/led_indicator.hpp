#pragma once

#include <cstdint>

#include "arming_sequence.hpp"
#include "led_sequencer.hpp"

namespace alc
{

  /** @brief The level of each of the three LEDs. */
  struct LedLevels
  {
      bool red;
      bool orange;
      bool green;

      bool operator==(const LedLevels&) const = default;
  };

  /**
   * @brief What the base level depends on. Gathered by App from the sanctioned
   *        state reads; never from INT1 or the AWAKE bit.
   */
  struct LedInputs
  {
      ArmState state;       ///< m_arming.State().
      bool clockValid;      ///< DeviceClock::IsValid() - set by the provisioner time sync.
      bool faultLatched;    ///< The device disarmed itself and no command has been accepted since.
      bool simulatedFiring; ///< The output would be asserted if armed: detection met, or a rehearsal FIRE firing.
  };

  // Disarmed with the clock set: green 500 ms on, 2500 ms off (owner, 2026-11-12).
  constexpr uint32_t M_GREEN_FLASH_ON_MS { 500 };
  constexpr uint32_t M_GREEN_FLASH_PERIOD_MS { 3000 };

  /**
   * @brief The LED levels the device state calls for when no overlay plays.
   *
   * First match wins (docs/superpowers/specs/2026-11-12-three-led-indication-design.md
   * sections 3.1 and 3.3):
   * - Active: all dark - nothing visible while armed.
   * - Arming: red steady. Simulated firing is masked.
   * - Inactive, fault latched: orange steady.
   * - Inactive, simulated firing: red, green off.
   * - Inactive, clock not set: green steady.
   * - Inactive, clock set: green slow flash.
   */
  LedLevels BaseLevels(const LedInputs& inputs, int64_t nowMs);

  /**
   * @brief The LED levels while an overlay plays: only the pattern's own LED, at
   *        `on`; the other two dark, so the overlay reads on its own (spec section 3.3).
   */
  LedLevels OverlayLevels(LedPattern pattern, bool on);

}
