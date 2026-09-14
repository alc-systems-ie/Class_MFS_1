#include <cassert>
#include <cstdio>
#include <initializer_list>

#include "arm_policy.hpp"

void run_arm_policy_tests()
{
  using namespace alc;

  constexpr bool M_BOTH_SLOT_KINDS[] { false, true };
  constexpr ArmState M_ALL_STATES[] { ArmState::Inactive, ArmState::Arming, ArmState::Active };
  protocol::Command command;
  ArmDecision decision;

  // Settings fields deliberately non-default, so a leak into Arm or Disarm shows.
  command.activations = 5;
  command.delayCode   = 127;
  command.mode        = protocol::Mode::ReportOnly;

  for (bool fromNetworkManager : M_BOTH_SLOT_KINDS) {
    // ARMED + Disarm: disarm and trim, nothing else.
    command.type = protocol::CommandType::Disarm;
    decision     = DecideCommand(ArmState::Active, fromNetworkManager, command);
    assert(decision.action == ArmAction::Disarm);
    assert(!decision.applySettings && !decision.applyMode && decision.trimClock);

    // ARMED + Arm or Settings: the state is unchanged, LED A replays Armed.
    // No settings, no mode, not even a trim - armed, only a disarm acts.
    for (protocol::CommandType type : { protocol::CommandType::Arm, protocol::CommandType::Settings }) {
      command.type = type;
      decision     = DecideCommand(ArmState::Active, fromNetworkManager, command);
      assert(decision.action == ArmAction::ReplayArmed);
      assert(!decision.applySettings && !decision.applyMode && !decision.trimClock);
    }

    // INACTIVE + Arm: arm with the STORED settings. The command's are not applied.
    command.type = protocol::CommandType::Arm;
    decision     = DecideCommand(ArmState::Inactive, fromNetworkManager, command);
    assert(decision.action == ArmAction::Arm);
    assert(!decision.applySettings && !decision.applyMode && decision.trimClock);

    // INACTIVE + Disarm: the ordinary deactivation, which also ends tuning.
    command.type = protocol::CommandType::Disarm;
    decision     = DecideCommand(ArmState::Inactive, fromNetworkManager, command);
    assert(decision.action == ArmAction::Disarm);
    assert(!decision.applySettings && !decision.applyMode && decision.trimClock);

    // ARMING + Disarm: cancels arming, with the trim (arming sequence amendment 3.1).
    command.type = protocol::CommandType::Disarm;
    decision     = DecideCommand(ArmState::Arming, fromNetworkManager, command);
    assert(decision.action == ArmAction::Disarm);
    assert(!decision.applySettings && !decision.applyMode && decision.trimClock);

    // ARMING + Arm or Settings: nothing at all - no replay, no settings, no mode, no trim.
    for (protocol::CommandType type : { protocol::CommandType::Arm, protocol::CommandType::Settings }) {
      command.type = type;
      decision     = DecideCommand(ArmState::Arming, fromNetworkManager, command);
      assert(decision.action == ArmAction::Ignore);
      assert(!decision.applySettings && !decision.applyMode && !decision.trimClock);
    }

    // RESERVED never acts in any state (decode rejects it first; defence in depth).
    command.type = protocol::CommandType::Reserved;
    for (ArmState state : M_ALL_STATES) {
      decision = DecideCommand(state, fromNetworkManager, command);
      assert(decision.action == ArmAction::Ignore);
      assert(!decision.applySettings && !decision.applyMode && !decision.trimClock);
    }
  }

  // INACTIVE + Settings: tune; mode only from slot 0.
  command.type = protocol::CommandType::Settings;
  decision     = DecideCommand(ArmState::Inactive, false, command);
  assert(decision.action == ArmAction::Tune && decision.applySettings && !decision.applyMode && decision.trimClock);
  decision = DecideCommand(ArmState::Inactive, true, command);
  assert(decision.action == ArmAction::Tune && decision.applySettings && decision.applyMode);

  printf("arm policy: OK\n");
}
