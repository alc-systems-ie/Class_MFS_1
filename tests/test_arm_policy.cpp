#include <cassert>
#include <cstdio>

#include "arm_policy.hpp"

void run_arm_policy_tests()
{
  using namespace alc;

  protocol::Command armCommand;
  protocol::Command disarmCommand;
  ArmDecision decision;
  constexpr bool M_BOTH_SLOT_KINDS[] { false, true };

  armCommand.armActive    = true;
  armCommand.activations  = 5;
  armCommand.delayCode    = 127;
  armCommand.mode         = protocol::Mode::ReportOnly;
  disarmCommand           = armCommand;
  disarmCommand.armActive = false;

  // ARMED + anything but disarm: NOTHING. Not even from the Network Manager, not
  // even a clock trim. There is one path out of the armed state on command.
  for (bool fromNetworkManager : M_BOTH_SLOT_KINDS) {
    decision = DecideCommand(true, fromNetworkManager, armCommand);
    assert(decision.action == ArmAction::Ignore);
    assert(!decision.applySettings && !decision.applyMode && !decision.trimClock);
  }

  // ARMED + disarm: disarm, and nothing else. The settings, delay and mode the
  // command carries are not applied - from any slot.
  for (bool fromNetworkManager : M_BOTH_SLOT_KINDS) {
    decision = DecideCommand(true, fromNetworkManager, disarmCommand);
    assert(decision.action == ArmAction::Disarm);
    assert(!decision.applySettings && !decision.applyMode);
    assert(decision.trimClock);
  }

  // INACTIVE + arm: arm with the command's settings; mode only from slot 0.
  decision = DecideCommand(false, false, armCommand);
  assert(decision.action == ArmAction::Arm && decision.applySettings && !decision.applyMode && decision.trimClock);
  decision = DecideCommand(false, true, armCommand);
  assert(decision.action == ArmAction::Arm && decision.applyMode);

  // INACTIVE + disarm bit: tune.
  decision = DecideCommand(false, false, disarmCommand);
  assert(decision.action == ArmAction::Tune && decision.applySettings && !decision.applyMode);
  decision = DecideCommand(false, true, disarmCommand);
  assert(decision.action == ArmAction::Tune && decision.applyMode);

  printf("arm policy: OK\n");
}
