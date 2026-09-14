#pragma once

#include <cstdint>

#include "arming_sequence.hpp"
#include "mfs_protocol.hpp"

namespace alc
{

  /** @brief What an accepted command is allowed to do. */
  enum class ArmAction : uint8_t {
    Ignore,      ///< NOTHING happens: a reserved type (decode already rejects it), or Arm/Settings while Arming.
    Disarm,      ///< -> Inactive. From Active the only state change a command can make; Arming: cancels; Inactive: restarts the test.
    Arm,         ///< Inactive -> Arming (then Active after the exit delay) with the STORED settings.
    Tune,        ///< Inactive stays Inactive; the command's settings applied and the test restarted.
    ReplayArmed, ///< Armed, and the command does not disarm. State unchanged; LED A replays Armed.
  };

  struct ArmDecision
  {
      ArmAction action { ArmAction::Ignore };
      bool applySettings { false };
      bool applyMode { false };
      bool trimClock { false };
  };

  /**
   * @brief THE SINGLE PATH. Decides what an accepted command may do. Pure.
   *
   * See the command types amendment section 2.2 and the arming sequence
   * amendment section 3.1 for the full tables. Summary:
   *
   * | Device is | Command  | Action                       | Settings | Mode | Trim |
   * |-----------|----------|-------------------------------|----------|------|------|
   * | Active    | Disarm   | Disarm                        | no       | no   | yes  |
   * | Active    | Arm      | No change - replay Armed      | no       | no   | no   |
   * | Active    | Settings | No change - replay Armed      | no       | no   | no   |
   * | Arming    | Disarm   | Disarm - cancels arming       | no       | no   | yes  |
   * | Arming    | Arm      | Ignore - nothing, no replay   | no       | no   | no   |
   * | Arming    | Settings | Ignore - nothing, no replay   | no       | no   | no   |
   * | Inactive  | Arm      | Arming, STORED settings       | no       | no   | yes  |
   * | Inactive  | Disarm   | Stay Inactive, restart test   | no       | no   | yes  |
   * | Inactive  | Settings | Tune                          | yes      | slot 0 only | yes |
   *
   * **An armed device has only two ways out of the armed state: a disarm
   * command, or firing** (App latches Inactive when the output period ends).
   * Power loss also leaves it Inactive, because cold start is Inactive. Arm and
   * Settings sent to an armed device change nothing but replay the Armed state
   * on LED A, so an engineer who does not know the state learns it.
   *
   * Arm and Disarm carry no settings: arming uses exactly the settings already
   * stored, never the command's own settings fields.
   *
   * App::applyCommand() must act on this decision and on nothing else.
   *
   * Arming is treated as armed for command policy, except that nothing is
   * replayed: LED A has not acknowledged the arm yet, so a replay would reveal
   * the state early and an Arm must not restart the exit delay.
   *
   * @param state              The device's arm state now.
   * @param fromNetworkManager Whether the command came from slot 0.
   */
  inline ArmDecision DecideCommand(ArmState state, bool fromNetworkManager, const protocol::Command& command)
  {
    ArmDecision decision {};

    if (command.type == protocol::CommandType::Reserved) { return decision; }

    if (command.type == protocol::CommandType::Disarm) {
      // Armed or not. From Inactive it is the ordinary deactivation, so a Disarm
      // sent blind to an Inactive device is harmless and restarts the test from zero.
      decision.action    = ArmAction::Disarm;
      decision.trimClock = true;
      return decision;
    }

    if (state == ArmState::Arming) {
      // Arm or Settings during the exit delay: nothing at all - no replay, no
      // settings, no mode, no trim, and the deadline is not moved.
      return decision;
    }

    if (state == ArmState::Active) {
      // Arm or Settings while armed: no settings, no mode, no trim, no re-arm.
      // LED A replays Armed so an engineer who did not know the state learns it.
      decision.action = ArmAction::ReplayArmed;
      return decision;
    }

    decision.trimClock = true;
    if (command.type == protocol::CommandType::Arm) {
      // The command carries no settings; arming uses those already stored.
      decision.action = ArmAction::Arm;
      return decision;
    }

    decision.action        = ArmAction::Tune;
    decision.applySettings = true;
    decision.applyMode     = fromNetworkManager;
    return decision;
  }

}
