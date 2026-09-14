#include <cassert>
#include <cstdio>

#include "scan_policy.hpp"

// Host tests for DesiredFastScan(). Clause references are to
// docs/superpowers/specs/2026-09-14-scan-reliability-amendment.md section 3.

void run_scan_policy_tests()
{
  using namespace alc;

  // Inactive: duty-cycled unless an armed delay is pending. That cannot happen
  // while Inactive in practice, but the arbiter is total.
  assert(!DesiredFastScan(ArmState::Inactive, false));
  assert(DesiredFastScan(ArmState::Inactive, true));

  // Arming: always continuous, so a Disarm during the exit delay is heard at once.
  assert(DesiredFastScan(ArmState::Arming, false));
  assert(DesiredFastScan(ArmState::Arming, true));

  // Active: continuous only while an armed trigger delay is pending.
  assert(!DesiredFastScan(ArmState::Active, false));
  assert(DesiredFastScan(ArmState::Active, true));

  printf("scan policy tests passed\n");
}
