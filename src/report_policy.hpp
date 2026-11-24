#pragma once

#include <cstdint>
#include <optional>

#include "arming_sequence.hpp"
#include "mfs_protocol.hpp"
#include "report_keys.hpp"

namespace alc
{

  /**
   * @brief Pure event-report policy: which events are due, and which are switched off.
   *
   * A disarmed device (Inactive or Arming) sends nothing. Activated is due on entering Active, Deactivated on leaving it, except
   * after a fire. Low battery is due once while Active, with hysteresis.
   */
  class ReportPolicy
  {
    public:
      static constexpr uint16_t M_LOW_BATTERY_MV { 2700 };
      static constexpr uint16_t M_BATTERY_RECOVERED_MV { 2800 };

      /** True if the suppress mask (settings byte 6, bits 0-3) leaves this event on. */
      static bool Enabled(report::ReportEvent event, uint8_t suppressMask);

      /** Call once per tick. The first call only establishes the previous state, since boot is not a transition. */
      std::optional<report::ReportEvent> ServiceArmState(ArmState state);

      /** Records that the device fired during the current Active session. */
      void NoteFired();

      void NoteBattery(uint16_t millivolts);

      /** True once while Active when a low reading is unreported. */
      bool TakeLowBattery(ArmState state);

    private:
      ArmState m_previous { ArmState::Inactive };
      bool m_have_previous { false };
      bool m_fired_this_session { false };
      bool m_battery_low { false };
      bool m_low_reported { false };
  };

}
