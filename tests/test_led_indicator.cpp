#include <cassert>
#include <cstdio>
#include <initializer_list>

#include "led_indicator.hpp"

void run_led_indicator_tests()
{
  using alc::ArmState;
  using alc::BaseLevels;
  using alc::LedInputs;
  using alc::LedLevels;
  using alc::LedPattern;
  using alc::OverlayLevels;

  constexpr LedLevels M_DARK { false, false, false };
  constexpr LedLevels M_RED { true, false, false };
  constexpr LedLevels M_ORANGE { false, true, false };
  constexpr LedLevels M_GREEN { false, false, true };

  // Disarmed, clock not set: steady green (every boot until the time sync).
  LedInputs bootInputs { ArmState::Inactive, false, false, false };
  assert(BaseLevels(bootInputs, 0) == M_GREEN);
  assert(BaseLevels(bootInputs, 1000) == M_GREEN);
  assert(BaseLevels(bootInputs, 2999) == M_GREEN);

  // Disarmed, clock set: green 500 ms on, 2500 ms off.
  LedInputs readyInputs { ArmState::Inactive, true, false, false };
  assert(BaseLevels(readyInputs, 0) == M_GREEN);
  assert(BaseLevels(readyInputs, 499) == M_GREEN);
  assert(BaseLevels(readyInputs, 500) == M_DARK);
  assert(BaseLevels(readyInputs, 2999) == M_DARK);
  assert(BaseLevels(readyInputs, 3000) == M_GREEN);
  assert(BaseLevels(readyInputs, 3499) == M_GREEN);
  assert(BaseLevels(readyInputs, 3500) == M_DARK);

  // Disarmed by the device itself: steady orange, whatever else is true.
  for (bool clockValid : { false, true }) {
    for (bool simulated : { false, true }) {
      LedInputs latched { ArmState::Inactive, clockValid, true, simulated };
      assert(BaseLevels(latched, 0) == M_ORANGE);
      assert(BaseLevels(latched, 1000) == M_ORANGE);
    }
  }

  // Simulated firing while disarmed, clock set: red, green off.
  LedInputs simulatedReady { ArmState::Inactive, true, false, true };
  assert(BaseLevels(simulatedReady, 0) == M_RED);
  assert(BaseLevels(simulatedReady, 1000) == M_RED);

  // Not before the clock is set (owner, 2026-11-12): a device waiting for its
  // time sync shows only steady green, whatever the accelerometer does.
  LedInputs simulatedBeforeSync { ArmState::Inactive, false, false, true };
  assert(BaseLevels(simulatedBeforeSync, 0) == M_GREEN);
  assert(BaseLevels(simulatedBeforeSync, 1000) == M_GREEN);

  // Arming: steady red, simulated firing masked.
  for (bool simulated : { false, true }) {
    LedInputs arming { ArmState::Arming, true, false, simulated };
    assert(BaseLevels(arming, 0) == M_RED);
    assert(BaseLevels(arming, 1000) == M_RED);
  }

  // Armed: nothing visible, whatever else is true.
  LedInputs armed { ArmState::Active, true, true, true };
  assert(BaseLevels(armed, 0) == M_DARK);
  LedInputs armedQuiet { ArmState::Active, false, false, false };
  assert(BaseLevels(armedQuiet, 0) == M_DARK);

  // An overlay lights only its own LED; the others, green included, are off.
  assert(OverlayLevels(LedPattern::RedFlash, true) == M_RED);
  assert(OverlayLevels(LedPattern::RedFlash, false) == M_DARK);
  assert(OverlayLevels(LedPattern::OrangeTriple, true) == M_ORANGE);
  assert(OverlayLevels(LedPattern::OrangeSteady, true) == M_ORANGE);
  assert(OverlayLevels(LedPattern::OrangeSteady, false) == M_DARK);
  assert(OverlayLevels(LedPattern::None, true) == M_DARK);

  // Battery-test liveness blink: lit for exactly onTicks ticks once every
  // periodTicks. A 100 ms blink at a 100 ms tick is one lit tick, not none
  // (final review 2026-11-12: the decrement used to run before the read).
  alc::LivenessBlink oneTick { 3, 1 };
  assert(!oneTick.Tick());
  assert(!oneTick.Tick());
  assert(oneTick.Tick());
  assert(!oneTick.Tick());
  assert(!oneTick.Tick());
  assert(oneTick.Tick());

  alc::LivenessBlink fiveTicks { 8, 5 };
  uint32_t lit { 0 };
  for (uint32_t tick = 0; tick < 8; tick++) {
    if (fiveTicks.Tick()) { lit++; }
  }
  // Ticks 8 to 12 are lit; the next period's first lit tick would be 16.
  for (uint32_t tick = 0; tick < 7; tick++) {
    if (fiveTicks.Tick()) { lit++; }
  }
  assert(lit == 5);
  assert(fiveTicks.Tick());

  alc::LivenessBlink never { 3, 0 };
  for (uint32_t tick = 0; tick < 10; tick++) {
    assert(!never.Tick());
  }

  printf("led indicator: OK\n");
}
