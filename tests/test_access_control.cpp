#include <cassert>
#include <cerrno>
#include <cstdio>
#include <cstring>

#include "access_control.hpp"
#include "access_vectors.hpp"

namespace
{
  using namespace alc;
  using Verdict = AccessControl::Verdict;

  constexpr uint32_t M_DAY_256_START { DeviceClock::M_EPOCH_UNIX + DeviceClock::M_DAY_BOUNDARY_OFFSET_SECS + 256U * DeviceClock::M_SECONDS_PER_DAY };
  // 05:00 UTC on day 256, at uptime zero.
  constexpr uint32_t M_SYNC_UNIX { M_DAY_256_START + 3600 };
  constexpr uint16_t M_MINUTE_0500 { 300 };

  struct PersistSpy
  {
      int calls { 0 };
      int failWith { 0 };
      AccessState last {};
  };

  int persistSpy(const AccessState& state, void* context)
  {
    PersistSpy* spy { static_cast<PersistSpy*>(context) };
    spy->calls++;
    if (spy->failWith != 0) { return spy->failWith; }
    spy->last = state;
    return 0;
  }

  // Builds what the engineer's app would send.
  void buildCommand(uint16_t day, uint8_t slot, uint32_t n, bool arm, uint16_t minute, uint8_t* onAir)
  {
    uint8_t dayKey[access::M_DAY_KEY_BYTES] {};
    uint8_t plaintext[protocol::M_PLAINTEXT_BYTES] {};
    protocol::Command command;

    command.armActive   = arm;
    command.activations = 3;
    command.minuteOfDay = minute;
    protocol::EncodeCommand(command, plaintext);
    assert(access::DeriveDayKey(access::vectors::M_SECRET, access::vectors::M_DEVICE_ID, day, slot, dayKey) == 0);
    assert(access::SealCommand(dayKey, access::vectors::M_DEVICE_ID, day, slot, n, plaintext, onAir) == 0);
  }

  DeviceClock syncedClock()
  {
    DeviceClock clock;
    assert(clock.ApplyProvisionerSync(M_SYNC_UNIX, 0) == DeviceClock::SyncResult::Applied);
    return clock;
  }
}

void run_access_control_tests()
{
  uint8_t onAir[protocol::M_UUID_BYTES] {};

  // No clock, no access - even for an authentic command.
  {
    PersistSpy spy;
    AccessControl access(access::vectors::M_DEVICE_ID, access::vectors::M_SECRET, &persistSpy, &spy);
    DeviceClock clock;
    buildCommand(256, 1, 0, true, M_MINUTE_0500, onAir);
    assert(access.Evaluate(onAir, sizeof(onAir), clock, 0).verdict == Verdict::ClockInvalid);
    assert(spy.calls == 0);
  }

  // Happy path, sequence handling and replay.
  {
    PersistSpy spy;
    AccessControl access(access::vectors::M_DEVICE_ID, access::vectors::M_SECRET, &persistSpy, &spy);
    DeviceClock clock { syncedClock() };

    buildCommand(256, 1, 0, true, M_MINUTE_0500, onAir);
    AccessControl::Evaluation evaluation { access.Evaluate(onAir, sizeof(onAir), clock, 0) };
    assert(evaluation.verdict == Verdict::Accepted);
    assert(evaluation.slot == 1 && evaluation.n == 0);
    assert(evaluation.command.armActive && evaluation.command.activations == 3);

    // Persisted BEFORE returning: the rollover to day 256, then next[1] = 1.
    assert(spy.calls == 2);
    assert(spy.last.day == 256 && spy.last.next[1] == 1);

    // The same bytes again - the phone advertises each command ~160 times. The
    // spent ID is no longer expected, so it is NotForUs: silent AND not counted.
    assert(access.Evaluate(onAir, sizeof(onAir), clock, 0).verdict == Verdict::NotForUs);
    assert(access.ConsecutiveFailures() == 0);

    // Desync: the phone sent 1..9 and none arrived. n = 10 is inside the window.
    buildCommand(256, 1, 10, false, M_MINUTE_0500, onAir);
    evaluation = access.Evaluate(onAir, sizeof(onAir), clock, 0);
    assert(evaluation.verdict == Verdict::Accepted && evaluation.n == 10);
    assert(access.State().next[1] == 11);

    // The skipped numbers are dead: n = 5 is now behind the window.
    buildCommand(256, 1, 5, false, M_MINUTE_0500, onAir);
    assert(access.Evaluate(onAir, sizeof(onAir), clock, 0).verdict == Verdict::NotForUs);

    // Window edge: next = 11 accepts 26 (11 + 15) but not 27.
    buildCommand(256, 1, 27, false, M_MINUTE_0500, onAir);
    assert(access.Evaluate(onAir, sizeof(onAir), clock, 0).verdict == Verdict::NotForUs);
    buildCommand(256, 1, 26, false, M_MINUTE_0500, onAir);
    assert(access.Evaluate(onAir, sizeof(onAir), clock, 0).verdict == Verdict::Accepted);

    // Slots are independent: slot 2 still starts at zero.
    buildCommand(256, 2, 0, false, M_MINUTE_0500, onAir);
    evaluation = access.Evaluate(onAir, sizeof(onAir), clock, 0);
    assert(evaluation.verdict == Verdict::Accepted && evaluation.slot == 2);
  }

  // Yesterday's key is dead, and a new day resets every slot.
  {
    PersistSpy spy;
    AccessControl access(access::vectors::M_DEVICE_ID, access::vectors::M_SECRET, &persistSpy, &spy);
    DeviceClock clock { syncedClock() };
    AccessState restored {};
    restored.day     = 256;
    restored.next[1] = 7;
    access.Restore(restored);

    buildCommand(255, 1, 7, true, M_MINUTE_0500, onAir);
    assert(access.Evaluate(onAir, sizeof(onAir), clock, 0).verdict == Verdict::NotForUs);

    // Restored state is honoured: n = 6 on day 256 is spent.
    buildCommand(256, 1, 6, true, M_MINUTE_0500, onAir);
    assert(access.Evaluate(onAir, sizeof(onAir), clock, 0).verdict == Verdict::NotForUs);

    // Tomorrow at 05:00: slot 1 is back at zero under the new key.
    constexpr int64_t M_TOMORROW { DeviceClock::M_SECONDS_PER_DAY };
    buildCommand(257, 1, 0, true, M_MINUTE_0500, onAir);
    assert(access.Evaluate(onAir, sizeof(onAir), clock, M_TOMORROW).verdict == Verdict::Accepted);
    assert(access.State().day == 257 && access.State().next[1] == 1);
    assert(clock.FloorDay() == 257);
  }

  // Freshness: authentic but held for 11 minutes is rejected AND NOT CONSUMED.
  {
    PersistSpy spy;
    AccessControl access(access::vectors::M_DEVICE_ID, access::vectors::M_SECRET, &persistSpy, &spy);
    DeviceClock clock { syncedClock() };
    buildCommand(256, 3, 0, true, M_MINUTE_0500 - 11, onAir);
    assert(access.Evaluate(onAir, sizeof(onAir), clock, 0).verdict == Verdict::Stale);
    assert(access.State().next[3] == 0);
    assert(access.ConsecutiveFailures() == 0);
  }

  // Persist before acting: a failed save is NOT an acceptance, and does not advance.
  {
    PersistSpy spy;
    AccessControl access(access::vectors::M_DEVICE_ID, access::vectors::M_SECRET, &persistSpy, &spy);
    DeviceClock clock { syncedClock() };
    buildCommand(256, 1, 0, true, M_MINUTE_0500, onAir);
    assert(access.Evaluate(onAir, sizeof(onAir), clock, 0).verdict == Verdict::Accepted); // rolls the day in
    spy.failWith = -EIO;
    buildCommand(256, 1, 1, true, M_MINUTE_0500, onAir);
    assert(access.Evaluate(onAir, sizeof(onAir), clock, 0).verdict == Verdict::PersistFailed);
    assert(access.State().next[1] == 1);
    spy.failWith = 0;
    assert(access.Evaluate(onAir, sizeof(onAir), clock, 0).verdict == Verdict::Accepted);
  }

  // Garbage never counts. Only an expected ID with a bad tag does.
  {
    PersistSpy spy;
    AccessControl access(access::vectors::M_DEVICE_ID, access::vectors::M_SECRET, &persistSpy, &spy);
    DeviceClock clock { syncedClock() };
    uint8_t garbage[protocol::M_UUID_BYTES] {};

    for (int attempt = 0; attempt < 100; attempt++) {
      garbage[0] = static_cast<uint8_t>(attempt);
      assert(access.Evaluate(garbage, sizeof(garbage), clock, 0).verdict == Verdict::NotForUs);
    }
    assert(access.ConsecutiveFailures() == 0);
  }

  // Same corrupted ID resent 100 times: AuthFailed every time, but only 1 failure counted.
  {
    PersistSpy spy;
    AccessControl access(access::vectors::M_DEVICE_ID, access::vectors::M_SECRET, &persistSpy, &spy);
    DeviceClock clock { syncedClock() };
    buildCommand(256, 1, 0, true, M_MINUTE_0500, onAir);
    onAir[protocol::M_OFFSET_TAG] ^= 0x01;
    for (int attempt = 0; attempt < 100; attempt++) {
      assert(access.Evaluate(onAir, sizeof(onAir), clock, 0).verdict == Verdict::AuthFailed);
    }
    assert(access.ConsecutiveFailures() == 1);
    assert(!access.IsLockedOut(0));
  }

  // Lockout requires 20 distinct expected IDs: slot 1 (n=0..15) + slot 2 (n=0..3), each with tag bit flipped.
  {
    PersistSpy spy;
    AccessControl access(access::vectors::M_DEVICE_ID, access::vectors::M_SECRET, &persistSpy, &spy);
    DeviceClock clock { syncedClock() };

    // Corrupt slot 1, n = 0..15 (16 failures).
    for (uint32_t n = 0; n < 16; n++) {
      buildCommand(256, 1, n, true, M_MINUTE_0500, onAir);
      onAir[protocol::M_OFFSET_TAG] ^= 0x01;
      assert(access.Evaluate(onAir, sizeof(onAir), clock, 0).verdict == Verdict::AuthFailed);
    }
    assert(access.ConsecutiveFailures() == 16);
    assert(!access.IsLockedOut(0));

    // Corrupt slot 2, n = 0..3 (4 more = 20 total).
    for (uint32_t n = 0; n < 4; n++) {
      buildCommand(256, 2, n, true, M_MINUTE_0500, onAir);
      onAir[protocol::M_OFFSET_TAG] ^= 0x01;
      assert(access.Evaluate(onAir, sizeof(onAir), clock, 0).verdict == Verdict::AuthFailed);
    }
    assert(access.IsLockedOut(0));
  }

  // The lockout doubles and caps at four hours (with 20 fresh distinct IDs each round).
  {
    PersistSpy spy;
    AccessControl access(access::vectors::M_DEVICE_ID, access::vectors::M_SECRET, &persistSpy, &spy);
    DeviceClock clock { syncedClock() };
    int64_t now { 0 };
    const uint32_t expectedSecs[] { 600, 1200, 2400, 4800, 9600, 14400, 14400 };

    for (uint32_t round = 0; round < 7; round++) {
      uint32_t lockoutSecs { expectedSecs[round] };

      // Restore with fresh sequence numbers for each round.
      AccessState restored {};
      restored.day     = 256;
      restored.next[1] = round * 16;
      restored.next[2] = round * 16;
      access.Restore(restored);

      // Corrupt slot 1, n = base..base+15 (16 failures).
      for (uint32_t n = 0; n < 16; n++) {
        buildCommand(256, 1, restored.next[1] + n, true, M_MINUTE_0500, onAir);
        onAir[protocol::M_OFFSET_TAG] ^= 0x01;
        assert(access.Evaluate(onAir, sizeof(onAir), clock, now).verdict == Verdict::AuthFailed);
      }

      // Corrupt slot 2, n = base..base+3 (4 more = 20 total).
      for (uint32_t n = 0; n < 4; n++) {
        buildCommand(256, 2, restored.next[2] + n, true, M_MINUTE_0500, onAir);
        onAir[protocol::M_OFFSET_TAG] ^= 0x01;
        assert(access.Evaluate(onAir, sizeof(onAir), clock, now).verdict == Verdict::AuthFailed);
      }

      assert(access.IsLockedOut(now + lockoutSecs - 1));
      assert(!access.IsLockedOut(now + lockoutSecs));
      now += lockoutSecs;
    }
  }

  // Failed rollover persist: state reverts, floor advances anyway.
  {
    PersistSpy spy;
    AccessControl access(access::vectors::M_DEVICE_ID, access::vectors::M_SECRET, &persistSpy, &spy);
    DeviceClock clock { syncedClock() };
    AccessState restored {};
    restored.day     = 255;
    restored.next[1] = 5;
    access.Restore(restored);

    spy.failWith = -EIO;
    buildCommand(256, 1, 0, true, M_MINUTE_0500, onAir);
    assert(access.Evaluate(onAir, sizeof(onAir), clock, 0).verdict == Verdict::PersistFailed);
    assert(access.State().day == 255 && access.State().next[1] == 5);
    assert(clock.FloorDay() == 256); // Floor advanced despite persist failure.

    spy.failWith = 0;
    assert(access.Evaluate(onAir, sizeof(onAir), clock, 0).verdict == Verdict::Accepted);
  }

  // today < state.day: clock is behind persisted day.
  {
    PersistSpy spy;
    AccessControl access(access::vectors::M_DEVICE_ID, access::vectors::M_SECRET, &persistSpy, &spy);
    AccessState restored {};
    restored.day = 257;
    access.Restore(restored);

    DeviceClock clock { syncedClock() }; // Day 256 from sync.
    buildCommand(256, 1, 0, true, M_MINUTE_0500, onAir);
    assert(access.Evaluate(onAir, sizeof(onAir), clock, 0).verdict == Verdict::ClockInvalid);
  }

  // Sequence limit: next[1] = UINT32_MAX - 16, window size 0 beyond limit.
  {
    PersistSpy spy;
    AccessControl access(access::vectors::M_DEVICE_ID, access::vectors::M_SECRET, &persistSpy, &spy);
    DeviceClock clock { syncedClock() };
    AccessState restored {};
    restored.day     = 256;
    restored.next[1] = UINT32_MAX - 16;
    access.Restore(restored);

    buildCommand(256, 1, UINT32_MAX - 16, true, M_MINUTE_0500, onAir);
    assert(access.Evaluate(onAir, sizeof(onAir), clock, 0).verdict == Verdict::Accepted);
    assert(access.State().next[1] == UINT32_MAX - 15);

    // Next window number wraps, so it is not in the window.
    buildCommand(256, 1, UINT32_MAX - 15, true, M_MINUTE_0500, onAir);
    assert(access.Evaluate(onAir, sizeof(onAir), clock, 0).verdict == Verdict::NotForUs);
  }

  // Malformed plaintext not consumed: build a command with an invalid minute (1440 > 1439).
  {
    PersistSpy spy;
    AccessControl access(access::vectors::M_DEVICE_ID, access::vectors::M_SECRET, &persistSpy, &spy);
    DeviceClock clock { syncedClock() };

    uint8_t dayKey[access::M_DAY_KEY_BYTES] {};
    uint8_t plaintext[protocol::M_PLAINTEXT_BYTES] {};
    assert(access::DeriveDayKey(access::vectors::M_SECRET, access::vectors::M_DEVICE_ID, 256, 1, dayKey) == 0);

    // Encode a command with minute 1440 (invalid). Plaintext[4:6] = 0xA0 0x05 (LE) = 1440.
    plaintext[0] = 0;    // armActive = false
    plaintext[1] = 0;    // activations = 0
    plaintext[2] = 0;    // cooldown = 0
    plaintext[3] = 0;    // sensitivity = 0
    plaintext[4] = 0xA0; // minute LE low byte = 0xA0
    plaintext[5] = 0x05; // minute LE high byte = 0x05; 0x05A0 = 1440 (invalid).
    plaintext[6] = 0;
    plaintext[7] = 0;
    assert(access::SealCommand(dayKey, access::vectors::M_DEVICE_ID, 256, 1, 0, plaintext, onAir) == 0);

    assert(access.Evaluate(onAir, sizeof(onAir), clock, 0).verdict == Verdict::Malformed);
    assert(access.State().next[1] == 0); // Not consumed.
    assert(access.ConsecutiveFailures() == 0);
  }

  // Advance(): an invalid clock is a no-op - nothing to adopt yet.
  {
    PersistSpy spy;
    AccessControl access(access::vectors::M_DEVICE_ID, access::vectors::M_SECRET, &persistSpy, &spy);
    DeviceClock clock;

    assert(access.Advance(clock, 0) == 0);
    assert(spy.calls == 0);
  }

  // Advance(): a stale floor is adopted with no command at all, and a repeat
  // call at the same uptime does not persist again.
  {
    PersistSpy spy;
    AccessControl access(access::vectors::M_DEVICE_ID, access::vectors::M_SECRET, &persistSpy, &spy);
    DeviceClock clock { syncedClock() }; // Day 256 at uptime 0.
    AccessState restored {};
    restored.day     = 250;
    restored.next[1] = 5;
    access.Restore(restored);

    assert(access.Advance(clock, 0) == 0);
    assert(spy.calls == 1);
    assert(access.State().day == 256);
    assert(access.State().next[1] == 0 && access.State().next[2] == 0);
    assert(clock.FloorDay() == 256);

    // Same day, same uptime - already adopted, so no second persist.
    assert(access.Advance(clock, 0) == 0);
    assert(spy.calls == 1);

    // A day later, with no command having been received in between.
    constexpr int64_t M_TOMORROW { DeviceClock::M_SECONDS_PER_DAY };
    assert(access.Advance(clock, M_TOMORROW) == 0);
    assert(spy.calls == 2);
    assert(access.State().day == 257);

    // Advance() alone rebuilt the tables - an authentic command for the new day
    // is still accepted.
    buildCommand(257, 1, 0, true, M_MINUTE_0500, onAir);
    assert(access.Evaluate(onAir, sizeof(onAir), clock, M_TOMORROW).verdict == Verdict::Accepted);
  }

  // Advance(): a failed persist leaves the state unchanged and reports the error.
  {
    PersistSpy spy;
    AccessControl access(access::vectors::M_DEVICE_ID, access::vectors::M_SECRET, &persistSpy, &spy);
    DeviceClock clock { syncedClock() }; // Day 256.
    AccessState restored {};
    restored.day = 250;
    access.Restore(restored);

    spy.failWith = -EIO;
    assert(access.Advance(clock, 0) == -EIO);
    assert(access.State().day == 250);
  }

  printf("access control: OK\n");
}
