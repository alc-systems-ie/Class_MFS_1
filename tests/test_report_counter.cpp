#include <cassert>
#include <cerrno>
#include <cstdint>
#include <cstdio>

#include "report_counter.hpp"

namespace
{
  struct FakeStore
  {
      uint32_t calls { 0 };
      uint32_t last { 0 };
      int failWith { 0 };
      uint32_t takesAtLastPersist { 0 };
      uint32_t* takeCounter { nullptr };
  };

  int fakePersist(uint32_t reservedUpTo, void* context)
  {
    FakeStore* store { static_cast<FakeStore*>(context) };

    store->calls++;
    if (store->takeCounter != nullptr) { store->takesAtLastPersist = *store->takeCounter; }
    if (store->failWith != 0) { return store->failWith; }
    store->last = reservedUpTo;
    return 0;
  }
}

void run_report_counter_tests()
{
  using alc::ReportCounter;

  // Begin persists the block before any value is usable.
  {
    FakeStore store {};
    ReportCounter counter { fakePersist, &store };
    uint32_t value { 99 };

    assert(!counter.Take(value));
    assert(counter.Begin(0) == 0);
    assert(store.calls == 1 && store.last == 16);
    for (uint32_t index { 0 }; index < 16; index++) {
      assert(counter.Take(value));
      assert(value == index);
    }
    assert(!counter.Take(value));
  }

  // A non-zero floor starts at the floor.
  {
    FakeStore store {};
    ReportCounter counter { fakePersist, &store };
    uint32_t value { 0 };

    assert(counter.Begin(32) == 0);
    assert(store.last == 48);
    assert(counter.Take(value) && value == 32);
  }

  // NeedsRefill after the 12th take; Refill persists first, then values continue.
  {
    FakeStore store {};
    ReportCounter counter { fakePersist, &store };
    uint32_t value { 0 };

    assert(counter.Begin(0) == 0);
    for (uint32_t index { 0 }; index < 11; index++) {
      assert(counter.Take(value));
    }
    assert(!counter.NeedsRefill());
    assert(counter.Take(value));
    assert(counter.NeedsRefill());
    assert(counter.Refill() == 0);
    assert(store.last == 32);
    assert(!counter.NeedsRefill());
    for (uint32_t expected { 12 }; expected < 32; expected++) {
      assert(counter.Take(value));
      assert(value == expected);
    }
    assert(!counter.Take(value));
  }

  // The reservation is persisted before the first value of a refilled block is taken.
  {
    FakeStore store {};
    ReportCounter counter { fakePersist, &store };
    uint32_t value { 0 };
    uint32_t taken { 0 };

    store.takeCounter = &taken;
    assert(counter.Begin(0) == 0);
    for (uint32_t index { 0 }; index < 16; index++) {
      assert(counter.Take(value));
      taken++;
    }
    assert(counter.Refill() == 0);
    assert(store.takesAtLastPersist == 16);
    assert(counter.Take(value) && value == 16 && store.last == 32);
  }

  // A failing persist in Begin leaves nothing usable.
  {
    FakeStore store {};
    ReportCounter counter { fakePersist, &store };
    uint32_t value { 0 };

    store.failWith = -EIO;
    assert(counter.Begin(0) == -EIO);
    assert(!counter.Take(value));
    assert(!counter.NeedsRefill());
    assert(counter.Refill() == -EINVAL);
  }

  // A failing Refill leaves the reserved values usable, and nothing beyond.
  {
    FakeStore store {};
    ReportCounter counter { fakePersist, &store };
    uint32_t value { 0 };

    assert(counter.Begin(0) == 0);
    for (uint32_t index { 0 }; index < 12; index++) {
      assert(counter.Take(value));
    }
    store.failWith = -EIO;
    assert(counter.Refill() == -EIO);
    assert(counter.NeedsRefill());
    for (uint32_t expected { 12 }; expected < 16; expected++) {
      assert(counter.Take(value) && value == expected);
    }
    assert(!counter.Take(value));

    // A later successful Refill resumes without repeating or skipping back.
    store.failWith = 0;
    assert(counter.Refill() == 0);
    assert(store.last == 32);
    assert(counter.Take(value) && value == 16);
  }

  // Simulated reset: the new Begin uses the stored floor, so no value repeats.
  {
    FakeStore store {};
    uint32_t highest { 0 };
    uint32_t value { 0 };
    bool any { false };

    for (uint32_t boot { 0 }; boot < 4; boot++) {
      ReportCounter counter { fakePersist, &store };

      assert(counter.Begin(store.last) == 0);
      for (uint32_t index { 0 }; index < 7; index++) {
        assert(counter.Take(value));
        assert(!any || value > highest);
        highest = value;
        any     = true;
        if (counter.NeedsRefill()) { assert(counter.Refill() == 0); }
      }
    }
  }

  // Overflow refuses rather than wrapping.
  {
    FakeStore store {};
    ReportCounter counter { fakePersist, &store };
    uint32_t value { 0 };

    assert(counter.Begin(0xFFFFFFF8u) == -EOVERFLOW);
    assert(store.calls == 0);
    assert(!counter.Take(value));

    assert(counter.Begin(0xFFFFFFEFu) == 0);
    assert(store.last == 0xFFFFFFFFu);
    assert(counter.Refill() == -EOVERFLOW);
    assert(store.calls == 1);
  }

  printf("report counter tests passed\n");
}
