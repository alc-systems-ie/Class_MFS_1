#include <cassert>
#include <cstdio>
#include <initializer_list>

#include "arm_policy.hpp"

void run_arm_policy_tests()
{
  using namespace alc;

  constexpr bool M_BOTH_SLOT_KINDS[] { false, true };
  protocol::Command command;
  ArmDecision decision;

  // Settings fields deliberately non-default, so a leak into Arm or Disarm shows.
  command.activations = 5;
  command.delayCode   = 127;
  command.mode        = protocol::Mode::ReportOnly;

  for (bool fromNetworkManager : M_BOTH_SLOT_KINDS) {
    // ARMED + Disarm: disarm and trim, nothing else.
    command.type = protocol::CommandType::Disarm;
    decision     = DecideCommand(true, fromNetworkManager, command);
    assert(decision.action == ArmAction::Disarm);
    assert(!decision.applySettings && !decision.applyMode && decision.trimClock);

    // ARMED + Arm or Settings: the state is unchanged, LED A replays Armed.
    // No settings, no mode, not even a trim - armed, only a disarm acts.
    for (protocol::CommandType type : { protocol::CommandType::Arm, protocol::CommandType::Settings }) {
      command.type = type;
      decision     = DecideCommand(true, fromNetworkManager, command);
      assert(decision.action == ArmAction::ReplayArmed);
      assert(!decision.applySettings && !decision.applyMode && !decision.trimClock);
    }

    // INACTIVE + Arm: arm with the STORED settings. The command's are not applied.
    command.type = protocol::CommandType::Arm;
    decision     = DecideCommand(false, fromNetworkManager, command);
    assert(decision.action == ArmAction::Arm);
    assert(!decision.applySettings && !decision.applyMode && decision.trimClock);

    // INACTIVE + Disarm: the ordinary deactivation, which also ends tuning.
    command.type = protocol::CommandType::Disarm;
    decision     = DecideCommand(false, fromNetworkManager, command);
    assert(decision.action == ArmAction::Disarm);
    assert(!decision.applySettings && !decision.applyMode && decision.trimClock);

    // RESERVED never acts, armed or not (decode rejects it first; defence in depth).
    command.type = protocol::CommandType::Reserved;
    for (bool armed : M_BOTH_SLOT_KINDS) {
      decision = DecideCommand(armed, fromNetworkManager, command);
      assert(decision.action == ArmAction::Ignore);
      assert(!decision.applySettings && !decision.applyMode && !decision.trimClock);
    }
  }

  // INACTIVE + Settings: tune; mode only from slot 0.
  command.type = protocol::CommandType::Settings;
  decision     = DecideCommand(false, false, command);
  assert(decision.action == ArmAction::Tune && decision.applySettings && !decision.applyMode && decision.trimClock);
  decision = DecideCommand(false, true, command);
  assert(decision.action == ArmAction::Tune && decision.applySettings && decision.applyMode);

  printf("arm policy: OK\n");
}
