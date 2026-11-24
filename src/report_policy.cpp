#include "report_policy.hpp"

#include "mfs_protocol.hpp"

namespace alc
{

  bool ReportPolicy::Enabled(report::ReportEvent event, uint8_t suppressMask)
  {
    uint8_t bit { 0 };

    switch (event) {
      case report::ReportEvent::LowBattery:
        bit = protocol::M_SUPPRESS_LOW_BATTERY;
        break;
      case report::ReportEvent::Activated:
        bit = protocol::M_SUPPRESS_ACTIVATED;
        break;
      case report::ReportEvent::Deactivated:
        bit = protocol::M_SUPPRESS_DEACTIVATED;
        break;
      case report::ReportEvent::FiredFromCommand:
      case report::ReportEvent::FiredFromMotion:
      case report::ReportEvent::FiredFromSound:
        bit = protocol::M_SUPPRESS_FIRED;
        break;
    }
    return (suppressMask & bit) == 0;
  }

  std::optional<report::ReportEvent> ReportPolicy::ServiceArmState(ArmState state)
  {
    std::optional<report::ReportEvent> due {};

    if (m_have_previous && state != m_previous) {
      if (state == ArmState::Active) {
        m_fired_this_session = false;
        due                  = report::ReportEvent::Activated;
      } else if (m_previous == ArmState::Active && !m_fired_this_session) {
        due = report::ReportEvent::Deactivated;
      }
    }
    m_previous      = state;
    m_have_previous = true;
    return due;
  }

  void ReportPolicy::NoteFired()
  { m_fired_this_session = true; }

  void ReportPolicy::NoteBattery(uint16_t millivolts)
  {
    if (millivolts < M_LOW_BATTERY_MV) {
      m_battery_low = true;
    } else if (millivolts >= M_BATTERY_RECOVERED_MV) {
      m_battery_low  = false;
      m_low_reported = false;
    }
  }

  bool ReportPolicy::TakeLowBattery(ArmState state)
  {
    if (state != ArmState::Active || !m_battery_low || m_low_reported) { return false; }
    m_low_reported = true;
    return true;
  }

}
