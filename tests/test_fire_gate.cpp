#include <cassert>
#include <cstdint>
#include <cstdio>

#include "fire_gate.hpp"

void run_fire_gate_tests()
{
  using alc::FireGate;

  // First true raises one announce and holds the output; it asserts at 100 ms.
  {
    FireGate gate {};
    assert(!gate.Update(true, 0));
    assert(gate.TakeAnnounceDue());
    assert(!gate.TakeAnnounceDue());
    assert(!gate.Update(true, 99));
    assert(!gate.TakeAnnounceDue());
    assert(gate.Update(true, 100));
    assert(gate.Update(true, 101));
    assert(!gate.TakeAnnounceDue());
  }

  // No condition: false, nothing announced.
  {
    FireGate gate {};
    assert(!gate.Update(false, 0));
    assert(!gate.TakeAnnounceDue());
  }

  // A falling condition resets: a new announce, permit only 100 ms after the new start.
  {
    FireGate gate {};
    assert(!gate.Update(true, 0));
    assert(gate.TakeAnnounceDue());
    assert(!gate.Update(false, 50));
    assert(!gate.Update(true, 60));
    assert(gate.TakeAnnounceDue());
    assert(!gate.Update(true, 159));
    assert(gate.Update(true, 160));
  }

  // A condition that fell after permit needs a fresh window.
  {
    FireGate gate {};
    gate.Update(true, 0);
    assert(gate.TakeAnnounceDue());
    assert(gate.Update(true, 100));
    assert(!gate.Update(false, 101));
    assert(!gate.Update(true, 102));
    assert(gate.TakeAnnounceDue());
    assert(!gate.Update(true, 150));
    assert(gate.Update(true, 202));
  }

  // Skip() after the announce permits at once, and keeps permitting while the condition holds.
  {
    FireGate gate {};
    assert(!gate.Update(true, 0));
    assert(gate.TakeAnnounceDue());
    gate.Skip();
    assert(gate.Update(true, 1));
    assert(gate.Update(true, 2));
  }

  // Skip() before the first Update() of the condition has no effect.
  {
    FireGate gate {};
    gate.Skip();
    assert(!gate.Update(true, 0));
    assert(gate.TakeAnnounceDue());
    assert(!gate.Update(true, 50));
  }

  // Skip() with no condition has no effect, and an earlier Skip() does not survive a fall.
  {
    FireGate gate {};
    gate.Update(true, 0);
    gate.Skip();
    assert(gate.Update(true, 1));
    assert(!gate.Update(false, 2));
    assert(!gate.Update(true, 3));
    assert(gate.TakeAnnounceDue());
    assert(!gate.Update(true, 4));
    assert(gate.Update(true, 103));
  }

  // Skip() on the same tick, after Update() returned false, permits the same-tick re-Update.
  {
    FireGate gate {};
    assert(!gate.Update(true, 10));
    gate.Skip();
    assert(gate.Update(true, 10));
  }

  // Time before the start is "not yet elapsed".
  {
    FireGate gate {};
    assert(!gate.Update(true, 1000));
    assert(!gate.Update(true, 500));
    assert(gate.Update(true, 1100));
  }

  std::printf("fire_gate tests passed\n");
}
