#pragma once

#include "arming_sequence.hpp"

// Pure: no Zephyr headers, so the arbiter compiles and is tested on the host.
//
// Design: docs/superpowers/specs/2026-09-14-scan-reliability-amendment.md section 3.

namespace alc
{

  /**
   * @brief The scanner cadence arbiter - the ONE place the cadence is decided.
   *
   * Continuous while Arming, so a Disarm sent during the exit delay is heard
   * within a fraction of a second of the phone advertising, and while an armed
   * trigger delay is pending. Duty-cycled otherwise. App::applyScanCadence() is
   * the one place the result is applied.
   *
   * @param state The arm state - ArmingSequence::State().
   * @param armedDelayPending Whether the detection engine has requested
   *        continuous scanning for an ARMED trigger delay.
   * @return true for continuous scanning, false for duty-cycled.
   */
  inline bool DesiredFastScan(ArmState state, bool armedDelayPending)
  {
    return (state == ArmState::Arming) || armedDelayPending;
  }

}
