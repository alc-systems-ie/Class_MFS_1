#include <cassert>
#include <cstdio>

#include "led_sequencer.hpp"

void run_led_sequencer_tests()
{
  using alc::ColourOf;
  using alc::LedColour;
  using alc::LedPattern;
  using alc::LedSequencer;

  constexpr int64_t M_START { 1000 };
  // Every pattern is framed by a dark gap before and after, so it reads the
  // same on a lit or a dark base level.
  constexpr int64_t M_GAP { LedSequencer::M_FRAME_GAP_MS };
  LedSequencer leds;

  assert(M_GAP == 300);
  assert(!leds.IsActive(M_START));
  assert(!leds.Level(M_START));

  // Red flash (arming completed, armed replay): dark gap, then 60 ms on /
  // 65 ms off for 3 s, then a dark gap.
  assert(leds.Start(LedPattern::RedFlash, M_START));
  assert(leds.IsActive(M_START));
  assert(!leds.Level(M_START));
  assert(!leds.Level(M_START + M_GAP - 1));
  assert(leds.Level(M_START + M_GAP));
  assert(leds.Level(M_START + M_GAP + 59));
  assert(!leds.Level(M_START + M_GAP + 60));
  assert(!leds.Level(M_START + M_GAP + 124));
  assert(leds.Level(M_START + M_GAP + 125));
  assert(!leds.Level(M_START + M_GAP + 3000));
  assert(leds.IsActive(M_START + M_GAP + 3000 + M_GAP - 1));
  assert(!leds.IsActive(M_START + M_GAP + 3000 + M_GAP));

  // Orange steady (arming refused, every fault): on for 3 s, framed.
  leds.Stop();
  assert(leds.Start(LedPattern::OrangeSteady, M_START));
  assert(!leds.Level(M_START + M_GAP - 1));
  assert(leds.Level(M_START + M_GAP));
  assert(leds.Level(M_START + M_GAP + 1500));
  assert(leds.Level(M_START + M_GAP + 2999));
  assert(!leds.Level(M_START + M_GAP + 3000));
  assert(leds.IsActive(M_START + M_GAP + 3000 + M_GAP - 1));
  assert(!leds.IsActive(M_START + M_GAP + 3000 + M_GAP));

  // Orange triple (accepted command confirmation): three 150 ms flashes 350 ms
  // apart within 1 s, framed.
  leds.Stop();
  assert(leds.Start(LedPattern::OrangeTriple, M_START));
  assert(leds.Level(M_START + M_GAP + 0));
  assert(leds.Level(M_START + M_GAP + 149));
  assert(!leds.Level(M_START + M_GAP + 150));
  assert(!leds.Level(M_START + M_GAP + 349));
  assert(leds.Level(M_START + M_GAP + 350));
  assert(!leds.Level(M_START + M_GAP + 500));
  assert(leds.Level(M_START + M_GAP + 700));
  assert(leds.Level(M_START + M_GAP + 849));
  assert(!leds.Level(M_START + M_GAP + 850));
  assert(!leds.Level(M_START + M_GAP + 999));
  assert(leds.IsActive(M_START + M_GAP + 1000 + M_GAP - 1));
  assert(!leds.IsActive(M_START + M_GAP + 1000 + M_GAP));

  // Each pattern names the one LED it plays on.
  assert(ColourOf(LedPattern::RedFlash) == LedColour::Red);
  assert(ColourOf(LedPattern::OrangeTriple) == LedColour::Orange);
  assert(ColourOf(LedPattern::OrangeSteady) == LedColour::Orange);
  assert(ColourOf(LedPattern::None) == LedColour::None);

  // Priority: a confirmation never cuts short a warning still playing.
  leds.Stop();
  assert(leds.Start(LedPattern::OrangeSteady, M_START));
  assert(!leds.Start(LedPattern::OrangeTriple, M_START + 500));
  assert(leds.Current() == LedPattern::OrangeSteady);
  assert(!leds.Start(LedPattern::RedFlash, M_START + 500));
  assert(leds.Current() == LedPattern::OrangeSteady);

  // A warning replaces a confirmation.
  leds.Stop();
  assert(leds.Start(LedPattern::OrangeTriple, M_START));
  assert(leds.Start(LedPattern::OrangeSteady, M_START + 100));
  assert(leds.Current() == LedPattern::OrangeSteady);

  // The same pattern restarts from its lead-in.
  leds.Stop();
  assert(leds.Start(LedPattern::OrangeTriple, M_START));
  assert(leds.Start(LedPattern::OrangeTriple, M_START + 100));
  assert(!leds.Level(M_START + 100 + M_GAP - 1));
  assert(leds.Level(M_START + 100 + M_GAP));

  // Red flash outranks a confirmation.
  leds.Stop();
  assert(leds.Start(LedPattern::OrangeTriple, M_START));
  assert(leds.Start(LedPattern::RedFlash, M_START + 100));
  assert(!leds.Start(LedPattern::OrangeTriple, M_START + 200));
  assert(leds.Current() == LedPattern::RedFlash);

  // Once a pattern has ended, any pattern starts.
  leds.Stop();
  assert(leds.Start(LedPattern::OrangeSteady, M_START));
  assert(leds.Start(LedPattern::OrangeTriple, M_START + M_GAP + 3000 + M_GAP));
  assert(leds.Current() == LedPattern::OrangeTriple);

  // A disarm cancels a red flash still playing, so a disarmed device never
  // shows the armed pattern and the disarm's confirmation can play.
  leds.Stop();
  assert(leds.Start(LedPattern::RedFlash, M_START));
  leds.Cancel(LedPattern::RedFlash);
  assert(!leds.IsActive(M_START + 100));
  assert(leds.Start(LedPattern::OrangeTriple, M_START + 100));

  // Cancel leaves any other pattern playing.
  assert(leds.Start(LedPattern::OrangeSteady, M_START + 200));
  leds.Cancel(LedPattern::RedFlash);
  assert(leds.Current() == LedPattern::OrangeSteady);
  assert(leds.IsActive(M_START + 300));

  // Orange rapid flash (counter mismatch, owner 2026-11-14): 100 ms on / 100 ms
  // off for 5 s, framed, on orange.
  assert(ColourOf(LedPattern::OrangeRapid) == LedColour::Orange);
  leds.Stop();
  assert(leds.Start(LedPattern::OrangeRapid, M_START));
  assert(!leds.Level(M_START + M_GAP - 1));
  assert(leds.Level(M_START + M_GAP));
  assert(leds.Level(M_START + M_GAP + 99));
  assert(!leds.Level(M_START + M_GAP + 100));
  assert(!leds.Level(M_START + M_GAP + 199));
  assert(leds.Level(M_START + M_GAP + 200));
  assert(leds.Level(M_START + M_GAP + 4800));
  assert(!leds.Level(M_START + M_GAP + 5000));
  assert(leds.IsActive(M_START + M_GAP + 5000 + M_GAP - 1));
  assert(!leds.IsActive(M_START + M_GAP + 5000 + M_GAP));

  // Priority: above a confirmation, below the red flash and the warning.
  leds.Stop();
  assert(leds.Start(LedPattern::OrangeRapid, M_START));
  assert(!leds.Start(LedPattern::OrangeTriple, M_START + 100));
  assert(leds.Current() == LedPattern::OrangeRapid);
  assert(leds.Start(LedPattern::RedFlash, M_START + 200));
  assert(!leds.Start(LedPattern::OrangeRapid, M_START + 300));
  assert(leds.Start(LedPattern::OrangeSteady, M_START + 400));
  assert(!leds.Start(LedPattern::OrangeRapid, M_START + 500));
  leds.Stop();
  assert(leds.Start(LedPattern::OrangeTriple, M_START));
  assert(leds.Start(LedPattern::OrangeRapid, M_START + 100));

  printf("led sequencer: OK\n");
}
