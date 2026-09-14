#include <cassert>
#include <cstdio>

#include "led_sequencer.hpp"

void run_led_sequencer_tests()
{
  using alc::LedPattern;
  using alc::LedSequencer;

  constexpr int64_t M_START { 1000 };
  // Every pattern is framed by a dark gap before and after, so it stays
  // visible on a bench build where LED A idles lit while Inactive.
  constexpr int64_t M_GAP { LedSequencer::M_FRAME_GAP_MS };
  LedSequencer leds;

  assert(M_GAP == 300);
  assert(!leds.IsActive(M_START));
  assert(!leds.Level(M_START));

  // Armed: dark gap, then 60 ms on / 65 ms off for 3 s, then a dark gap.
  leds.Start(LedPattern::Armed, M_START);
  assert(leds.IsActive(M_START));
  assert(!leds.Level(M_START));             // lead-in is dark
  assert(!leds.Level(M_START + M_GAP - 1)); // still dark
  assert(leds.Level(M_START + M_GAP));      // first on phase
  assert(leds.Level(M_START + M_GAP + 59));
  assert(!leds.Level(M_START + M_GAP + 60));
  assert(!leds.Level(M_START + M_GAP + 124));
  assert(leds.Level(M_START + M_GAP + 125));
  assert(!leds.Level(M_START + M_GAP + 3000)); // trailing gap is dark
  assert(leds.IsActive(M_START + M_GAP + 3000 + M_GAP - 1));
  assert(!leds.IsActive(M_START + M_GAP + 3000 + M_GAP));

  // Double blink each second: on 0-99, off 100-199, on 200-299, off 300-999.
  leds.Start(LedPattern::DisarmedDelayCancelled, M_START);
  assert(leds.Level(M_START + M_GAP + 50));
  assert(!leds.Level(M_START + M_GAP + 150));
  assert(leds.Level(M_START + M_GAP + 250));
  assert(!leds.Level(M_START + M_GAP + 500));
  assert(leds.Level(M_START + M_GAP + 1050));

  // Settings applied: THE CASE THAT WAS INVISIBLE ON THE BENCH. On an LED that
  // idles lit, a bare 200 ms on phase changes nothing; framed, it reads as
  // off - blink - off, then back to the idle level.
  leds.Start(LedPattern::SettingsApplied, M_START);
  assert(!leds.Level(M_START + 100));
  assert(leds.Level(M_START + M_GAP + 100));
  assert(!leds.Level(M_START + M_GAP + 200 + 100));
  assert(leds.IsActive(M_START + M_GAP + 200 + M_GAP - 1));
  assert(!leds.IsActive(M_START + M_GAP + 200 + M_GAP));

  // Mode changed: two 200 ms blinks, framed.
  leds.Start(LedPattern::ModeChanged, M_START);
  assert(leds.Level(M_START + M_GAP + 100));
  assert(!leds.Level(M_START + M_GAP + 300));
  assert(leds.Level(M_START + M_GAP + 500));
  assert(!leds.Level(M_START + M_GAP + 600 + 100));
  assert(!leds.IsActive(M_START + M_GAP + 600 + M_GAP));

  // Warning (LED B, interim): dark gap, then 700 ms on / 300 ms off for 3 s -
  // three long pulses - then a dark gap.
  leds.Start(LedPattern::Warning, M_START);
  assert(!leds.Level(M_START + M_GAP - 1));
  assert(leds.Level(M_START + M_GAP));
  assert(leds.Level(M_START + M_GAP + 699));
  assert(!leds.Level(M_START + M_GAP + 700));
  assert(!leds.Level(M_START + M_GAP + 999));
  assert(leds.Level(M_START + M_GAP + 1000));
  assert(leds.Level(M_START + M_GAP + 2699));
  assert(!leds.Level(M_START + M_GAP + 2700));
  assert(!leds.Level(M_START + M_GAP + 3000));
  assert(leds.IsActive(M_START + M_GAP + 3000 + M_GAP - 1));
  assert(!leds.IsActive(M_START + M_GAP + 3000 + M_GAP));

  // A new warning replaces one already playing: the pattern restarts from its lead-in.
  leds.Start(LedPattern::Warning, M_START + 2000);
  assert(!leds.Level(M_START + 2000 + M_GAP - 1));
  assert(leds.Level(M_START + 2000 + M_GAP));
  assert(leds.IsActive(M_START + 2000 + M_GAP + 3000 + M_GAP - 1));

  // A new command replaces the pattern playing.
  leds.Start(LedPattern::Disarmed, M_START);
  leds.Start(LedPattern::SettingsApplied, M_START + 100);
  assert(leds.Current() == LedPattern::SettingsApplied);
  assert(!leds.Level(M_START + 250));
  assert(leds.Level(M_START + 100 + M_GAP + 50));

  printf("led sequencer: OK\n");
}
