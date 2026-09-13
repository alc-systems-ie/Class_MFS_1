#include <cassert>
#include <cstdio>

#include "led_sequencer.hpp"

void run_led_sequencer_tests()
{
  using alc::LedPattern;
  using alc::LedSequencer;

  constexpr int64_t M_START { 1000 };
  LedSequencer leds;

  assert(!leds.IsActive(M_START));
  assert(!leds.Level(M_START));

  // Armed: 60 ms on, 65 ms off, for 3 s.
  leds.Start(LedPattern::Armed, M_START);
  assert(leds.Level(M_START));
  assert(leds.Level(M_START + 59));
  assert(!leds.Level(M_START + 60));
  assert(!leds.Level(M_START + 124));
  assert(leds.Level(M_START + 125));
  assert(leds.IsActive(M_START + 2999));
  assert(!leds.IsActive(M_START + 3000));
  assert(!leds.Level(M_START + 3000));

  // Double blink each second: on 0-99, off 100-199, on 200-299, off 300-999.
  leds.Start(LedPattern::DisarmedDelayCancelled, M_START);
  assert(leds.Level(M_START + 50));
  assert(!leds.Level(M_START + 150));
  assert(leds.Level(M_START + 250));
  assert(!leds.Level(M_START + 500));
  assert(leds.Level(M_START + 1050));

  // Mode changed: two 200 ms blinks, then done at 600 ms.
  leds.Start(LedPattern::ModeChanged, M_START);
  assert(leds.Level(M_START + 100));
  assert(!leds.Level(M_START + 300));
  assert(leds.Level(M_START + 500));
  assert(!leds.IsActive(M_START + 600));

  // A new command replaces the pattern playing.
  leds.Start(LedPattern::Disarmed, M_START);
  leds.Start(LedPattern::SettingsApplied, M_START + 100);
  assert(leds.Current() == LedPattern::SettingsApplied);
  assert(leds.Level(M_START + 250));
  assert(!leds.IsActive(M_START + 300));

  printf("led sequencer: OK\n");
}
