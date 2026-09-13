#include <cassert>
#include <cstdio>

#include "device_clock.hpp"

namespace
{
  using alc::DeviceClock;

  // Day 256 starts at 04:00 UTC on 2026-09-14.
  constexpr uint32_t M_DAY_256_START { DeviceClock::M_EPOCH_UNIX + DeviceClock::M_DAY_BOUNDARY_OFFSET_SECS + 256U * DeviceClock::M_SECONDS_PER_DAY };
  constexpr uint32_t M_HOUR { 3600 };
  constexpr uint32_t M_MINUTE { 60 };
}

void run_device_clock_tests()
{
  uint16_t day { 0 };

  // Day index arithmetic, including the guard against underflow.
  assert(!DeviceClock::DayIndexOf(DeviceClock::M_EPOCH_UNIX, day)); // 00:00, before the first 04:00 boundary
  assert(DeviceClock::DayIndexOf(DeviceClock::M_EPOCH_UNIX + DeviceClock::M_DAY_BOUNDARY_OFFSET_SECS, day) && day == 0);
  assert(DeviceClock::DayIndexOf(M_DAY_256_START, day) && day == 256);
  assert(DeviceClock::DayIndexOf(M_DAY_256_START - 1, day) && day == 255); // 03:59:59 is still yesterday

  // Minute differences wrap across midnight to the nearest match.
  assert(DeviceClock::MinuteDifference(1, 1439) == 2);
  assert(DeviceClock::MinuteDifference(1439, 1) == -2);
  assert(DeviceClock::MinuteDifference(600, 590) == 10);
  assert(DeviceClock::MinuteDifference(0, 720) == -720);

  // A fresh boot is INVALID. There is no resume from NVS.
  {
    DeviceClock clock;
    assert(!clock.IsValid());
    assert(!clock.IsFresh(0, 0));
    assert(clock.ApplyMinuteHint(0, 0) == DeviceClock::TrimResult::ClockInvalid);
  }

  // Factory case: no floor, any post-epoch time is accepted.
  {
    DeviceClock clock;
    constexpr int64_t M_UPTIME { 1000 };
    assert(clock.ApplyProvisionerSync(DeviceClock::M_EPOCH_UNIX - 1, M_UPTIME) == DeviceClock::SyncResult::BeforeEpoch);
    assert(clock.ApplyProvisionerSync(M_DAY_256_START + M_HOUR, M_UPTIME) == DeviceClock::SyncResult::Applied);
    assert(clock.IsValid());
    assert(clock.NowUnix(M_UPTIME) == M_DAY_256_START + M_HOUR);
    assert(clock.NowUnix(M_UPTIME + 10) == M_DAY_256_START + M_HOUR + 10);
    assert(clock.DayIndex(M_UPTIME) == 256);
    assert(clock.MinuteOfDay(M_UPTIME) == 5 * 60); // 05:00 UTC
    assert(clock.HasFloor() && clock.FloorDay() == 256);

    // A valid clock refuses further syncs, however authentic. A captured sync
    // released later would otherwise pull the day back.
    assert(clock.ApplyProvisionerSync(M_DAY_256_START, M_UPTIME) == DeviceClock::SyncResult::AlreadyValid);
  }

  // The floor: never below it, never too far past it.
  {
    DeviceClock clock;
    clock.RaiseFloorDay(256);
    clock.RaiseFloorDay(10); // never lowers
    assert(clock.FloorDay() == 256);
    assert(clock.ApplyProvisionerSync(M_DAY_256_START - 1, 0) == DeviceClock::SyncResult::BelowFloor);
    assert(clock.ApplyProvisionerSync(M_DAY_256_START + 401U * DeviceClock::M_SECONDS_PER_DAY, 0) == DeviceClock::SyncResult::TooFarAhead);
    assert(clock.ApplyProvisionerSync(M_DAY_256_START + 400U * DeviceClock::M_SECONDS_PER_DAY, 0) == DeviceClock::SyncResult::Applied);
  }

  // Freshness and trimming.
  {
    DeviceClock clock;
    constexpr uint16_t M_OWN_MINUTE { 5 * 60 }; // 05:00
    assert(clock.ApplyProvisionerSync(M_DAY_256_START + M_HOUR, 0) == DeviceClock::SyncResult::Applied);

    assert(clock.IsFresh(M_OWN_MINUTE + 10, 0));
    assert(!clock.IsFresh(M_OWN_MINUTE + 11, 0));
    assert(clock.IsFresh(M_OWN_MINUTE - 10, 0));

    // Deadband: one minute is noise.
    assert(clock.ApplyMinuteHint(M_OWN_MINUTE + 1, 0) == DeviceClock::TrimResult::Unchanged);
    assert(clock.NowUnix(0) == M_DAY_256_START + M_HOUR);

    // Beyond five minutes but still fresh: IGNORED, not clamped.
    assert(clock.ApplyMinuteHint(M_OWN_MINUTE + 6, 0) == DeviceClock::TrimResult::Ignored);
    assert(clock.NowUnix(0) == M_DAY_256_START + M_HOUR);

    // Three minutes: applied.
    assert(clock.ApplyMinuteHint(M_OWN_MINUTE + 3, 0) == DeviceClock::TrimResult::Trimmed);
    assert(clock.NowUnix(0) == M_DAY_256_START + M_HOUR + 3 * M_MINUTE);

    // The daily budget counts magnitude: 3 used, so -3 would make 6 > 5.
    assert(clock.ApplyMinuteHint(M_OWN_MINUTE, 0) == DeviceClock::TrimResult::OverBudget);
    assert(clock.ApplyMinuteHint(M_OWN_MINUTE + 3 - 2, 0) == DeviceClock::TrimResult::Trimmed); // -2, total 5
    assert(clock.ApplyMinuteHint(M_OWN_MINUTE + 1 + 2, 0) == DeviceClock::TrimResult::OverBudget);

    // A new day index gets a new budget.
    constexpr int64_t M_NEXT_DAY_UPTIME { DeviceClock::M_SECONDS_PER_DAY };
    uint16_t minuteTomorrow { clock.MinuteOfDay(M_NEXT_DAY_UPTIME) };
    assert(clock.DayIndex(M_NEXT_DAY_UPTIME) == 257);
    assert(clock.ApplyMinuteHint(static_cast<uint16_t>(minuteTomorrow + 2), M_NEXT_DAY_UPTIME) == DeviceClock::TrimResult::Trimmed);
  }

  // A trim may never take the day index below the floor.
  {
    DeviceClock clock;
    // 04:02 on day 256: a -3 minute trim would land at 03:59, day 255.
    assert(clock.ApplyProvisionerSync(M_DAY_256_START + 2 * M_MINUTE, 0) == DeviceClock::SyncResult::Applied);
    assert(clock.ApplyMinuteHint(static_cast<uint16_t>(4 * 60 - 1), 0) == DeviceClock::TrimResult::WouldRewindDay);
    assert(clock.DayIndex(0) == 256);
  }

  printf("device clock: OK\n");
}
