#pragma once

#include <cstdint>

#include "mfs_protocol.hpp"

namespace alc
{

  /** @brief What an accepted command is allowed to do. */
  enum class ArmAction : uint8_t {
    Ignore, ///< Armed, and the command does not disarm. NOTHING happens.
    Disarm, ///< Armed -> Inactive. The only thing an armed device will do on command.
    Arm,    ///< Inactive -> Active with the command's settings.
    Tune,   ///< Inactive stays Inactive; settings applied for tuning.
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
   * **An armed device does exactly one thing on command: disarm.** A command with
   * the arm bit set is ignored outright - no settings, no mode, no clock trim,
   * no re-arm. A disarm applies nothing but the disarm: the settings, delay and
   * mode it carries are ignored, and the engineer sends settings once Inactive.
   *
   * An armed device therefore leaves the armed state only two ways: a disarm
   * command, or firing (App latches Inactive when the output period ends). Power
   * loss also leaves it Inactive, because cold start is Inactive.
   *
   * App::applyCommand() must act on this decision and on nothing else.
   *
   * @param armed              Whether the device is Active now.
   * @param fromNetworkManager Whether the command came from slot 0.
   */
  inline ArmDecision DecideCommand(bool armed, bool fromNetworkManager, const protocol::Command& command)
  {
    ArmDecision decision {};

    if (armed) {
      if (command.armActive) { return decision; }

      // The clock trim is the only side effect a disarm keeps. It changes no
      // device behaviour, and the command is authentic and fresh.
      decision.action    = ArmAction::Disarm;
      decision.trimClock = true;
      return decision;
    }

    decision.action        = command.armActive ? ArmAction::Arm : ArmAction::Tune;
    decision.applySettings = true;
    decision.applyMode     = fromNetworkManager;
    decision.trimClock     = true;
    return decision;
  }

}
