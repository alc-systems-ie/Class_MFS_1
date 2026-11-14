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
      bool lowBattery;      ///< Arming was refused by the fire-supply battery test and no command has been accepted since.
  };

  // Disarmed with the clock set: green 500 ms on, 2500 ms off (owner, 2026-11-12).
  constexpr uint32_t M_GREEN_FLASH_ON_MS { 500 };
  constexpr uint32_t M_GREEN_FLASH_PERIOD_MS { 3000 };

  // Arming refused for a low battery: orange 500 ms on, 500 ms off (owner, 2026-11-14).
  constexpr uint32_t M_LOW_BATTERY_FLASH_ON_MS { 500 };
  constexpr uint32_t M_LOW_BATTERY_FLASH_PERIOD_MS { 1000 };

  /**
   * @brief The LED levels the device state calls for when no overlay plays.
   *
   * First match wins (docs/superpowers/specs/2026-11-12-three-led-indication-design.md
   * sections 3.1 and 3.3):
   * - Active: all dark - nothing visible while armed.
   * - Arming: red steady. Simulated firing is masked.
   * - Inactive, low battery latched: orange slow flash (1 Hz).
   * - Inactive, fault latched: orange steady.
   * - Inactive, clock not set: green steady - simulated firing is not shown
   *   until the time sync.
   * - Inactive, simulated firing: red, green off.
   * - Inactive, clock set: green slow flash.
   */
  LedLevels BaseLevels(const LedInputs& inputs, int64_t nowMs);

  /**
   * @brief The LED levels while an overlay plays: only the pattern's own LED, at
   *        `on`; the other two dark, so the overlay reads on its own (spec section 3.3).
   */
  LedLevels OverlayLevels(LedPattern pattern, bool on);

  /**
   * @brief The battery-test liveness blink (CONFIG_MFS_BATTERY_TEST): lit for
   *        exactly `onTicks` main-loop ticks once every `periodTicks`.
   *
   * Call Tick() once per main-loop tick; it returns the level for that tick.
   * `onTicks` of 0 never lights.
   */
  class LivenessBlink
  {
    public:
      LivenessBlink(uint32_t periodTicks, uint32_t onTicks)
          : m_period_ticks(periodTicks)
          , m_on_ticks(onTicks)
          , m_ticks(0)
          , m_remaining_ticks(0)
      {}

      bool Tick();

    private:
      const uint32_t m_period_ticks;
      const uint32_t m_on_ticks;
      uint32_t m_ticks;
      uint32_t m_remaining_ticks;
  };

}
