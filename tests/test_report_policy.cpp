#include <cassert>
#include <cstdint>
#include <cstdio>

#include "report_policy.hpp"
#include "mfs_protocol.hpp"

void run_report_policy_tests()
{
  using alc::ArmState;
  using alc::ReportPolicy;
  using alc::report::ReportEvent;

  // The first call establishes the state: boot is not a transition.
  {
    ReportPolicy policy {};
    assert(!policy.ServiceArmState(ArmState::Active).has_value());
    assert(!policy.ServiceArmState(ArmState::Active).has_value());
  }

  // Inactive -> Arming -> Active gives one Activated, on the Active tick only.
  {
    ReportPolicy policy {};
    assert(!policy.ServiceArmState(ArmState::Inactive).has_value());
    assert(!policy.ServiceArmState(ArmState::Arming).has_value());
    assert(policy.ServiceArmState(ArmState::Active) == ReportEvent::Activated);
    assert(!policy.ServiceArmState(ArmState::Active).has_value());
  }

  // Active -> Inactive gives Deactivated.
  {
    ReportPolicy policy {};
    policy.ServiceArmState(ArmState::Inactive);
    policy.ServiceArmState(ArmState::Active);
    assert(policy.ServiceArmState(ArmState::Inactive) == ReportEvent::Deactivated);
    assert(!policy.ServiceArmState(ArmState::Inactive).has_value());
  }

  // A fire suppresses the Deactivated report.
  {
    ReportPolicy policy {};
    policy.ServiceArmState(ArmState::Inactive);
    policy.ServiceArmState(ArmState::Active);
    policy.NoteFired();
    assert(!policy.ServiceArmState(ArmState::Inactive).has_value());
  }

  // A cancelled arming gives nothing.
  {
    ReportPolicy policy {};
    policy.ServiceArmState(ArmState::Inactive);
    assert(!policy.ServiceArmState(ArmState::Arming).has_value());
    assert(!policy.ServiceArmState(ArmState::Inactive).has_value());
  }

  // The fired flag is cleared on the next Activated.
  {
    ReportPolicy policy {};
    policy.ServiceArmState(ArmState::Inactive);
    policy.ServiceArmState(ArmState::Active);
    policy.NoteFired();
    policy.ServiceArmState(ArmState::Inactive);
    assert(policy.ServiceArmState(ArmState::Active) == ReportEvent::Activated);
    assert(policy.ServiceArmState(ArmState::Inactive) == ReportEvent::Deactivated);
  }

  // NoteFired outside Active does not affect a later session.
  {
    ReportPolicy policy {};
    policy.ServiceArmState(ArmState::Inactive);
    policy.NoteFired();
    policy.ServiceArmState(ArmState::Active);
    assert(policy.ServiceArmState(ArmState::Inactive) == ReportEvent::Deactivated);
  }

  // Low battery is sent once, only while Active.
  {
    ReportPolicy policy {};
    assert(!policy.TakeLowBattery(ArmState::Active));
    policy.NoteBattery(2650);
    assert(!policy.TakeLowBattery(ArmState::Inactive));
    assert(!policy.TakeLowBattery(ArmState::Arming));
    assert(policy.TakeLowBattery(ArmState::Active));
    assert(!policy.TakeLowBattery(ArmState::Active));
  }

  // Hysteresis: 2750 does not re-arm, 2800 does.
  {
    ReportPolicy policy {};
    policy.NoteBattery(2650);
    assert(policy.TakeLowBattery(ArmState::Active));
    policy.NoteBattery(2750);
    policy.NoteBattery(2650);
    assert(!policy.TakeLowBattery(ArmState::Active));
    policy.NoteBattery(2800);
    policy.NoteBattery(2650);
    assert(policy.TakeLowBattery(ArmState::Active));
  }

  // A healthy reading never reports.
  {
    ReportPolicy policy {};
    policy.NoteBattery(2700);
    assert(!policy.TakeLowBattery(ArmState::Active));
  }

  // Suppress switches.
  {
    using namespace alc::protocol;
    assert(!ReportPolicy::Enabled(ReportEvent::FiredFromSound, M_SUPPRESS_FIRED));
    assert(!ReportPolicy::Enabled(ReportEvent::FiredFromCommand, M_SUPPRESS_FIRED));
    assert(!ReportPolicy::Enabled(ReportEvent::FiredFromMotion, M_SUPPRESS_FIRED));
    assert(ReportPolicy::Enabled(ReportEvent::Activated, M_SUPPRESS_FIRED));
    assert(!ReportPolicy::Enabled(ReportEvent::LowBattery, M_SUPPRESS_LOW_BATTERY));
    assert(!ReportPolicy::Enabled(ReportEvent::Activated, M_SUPPRESS_ACTIVATED));
    assert(!ReportPolicy::Enabled(ReportEvent::Deactivated, M_SUPPRESS_DEACTIVATED));
    assert(ReportPolicy::Enabled(ReportEvent::Deactivated, M_SUPPRESS_ACTIVATED));

    constexpr ReportEvent ALL[] { ReportEvent::LowBattery,       ReportEvent::Activated,       ReportEvent::Deactivated,
                                  ReportEvent::FiredFromCommand, ReportEvent::FiredFromMotion, ReportEvent::FiredFromSound };
    for (ReportEvent event : ALL) {
      assert(ReportPolicy::Enabled(event, 0));
    }
  }

  printf("report policy tests passed\n");
}
