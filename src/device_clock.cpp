#include <cstdlib>

#include "device_clock.hpp"

namespace alc
{

  DeviceClock::DeviceClock()
      : m_valid(false)
      , m_offset_secs(0)
      , m_has_floor(false)
      , m_floor_day(0)
      , m_trim_day(0)
      , m_trim_used_secs(0)
  {}

  bool DeviceClock::DayIndexOf(uint32_t unixSeconds, uint16_t& day)
  {
    // Guard the subtraction: a time before the epoch's first boundary would
    // underflow into an enormous day index.
    if (unixSeconds < M_EPOCH_UNIX + M_DAY_BOUNDARY_OFFSET_SECS) { return false; }

    day = static_cast<uint16_t>((unixSeconds - M_EPOCH_UNIX - M_DAY_BOUNDARY_OFFSET_SECS) / M_SECONDS_PER_DAY);
    return true;
  }

  int16_t DeviceClock::MinuteDifference(uint16_t presentedMinute, uint16_t ownMinute)
  {
    constexpr int16_t M_HALF_DAY_MINUTES { M_MINUTES_PER_DAY / 2 };
    int16_t difference { static_cast<int16_t>(static_cast<int16_t>(presentedMinute) - static_cast<int16_t>(ownMinute)) };

    // Nearest match across midnight: 23:59 against 00:01 is -2, not +1438.
    if (difference >= M_HALF_DAY_MINUTES) { difference = static_cast<int16_t>(difference - M_MINUTES_PER_DAY); }
    if (difference < -M_HALF_DAY_MINUTES) { difference = static_cast<int16_t>(difference + M_MINUTES_PER_DAY); }
    return difference;
  }

  void DeviceClock::RaiseFloorDay(uint16_t day)
  {
    if (!m_has_floor || day > m_floor_day) { m_floor_day = day; }
    m_has_floor = true;
  }

  uint32_t DeviceClock::NowUnix(int64_t uptimeSecs) const
  {
    return static_cast<uint32_t>(uptimeSecs + m_offset_secs);
  }

  uint16_t DeviceClock::DayIndex(int64_t uptimeSecs) const
  {
    uint16_t day { 0 };

    if (!DayIndexOf(NowUnix(uptimeSecs), day)) { day = 0; }

    // Belt and braces. Sync and trim both refuse to go below the floor, so this
    // should never bind - but the day index must never move backwards.
    if (m_has_floor && day < m_floor_day) { day = m_floor_day; }
    return day;
  }

  uint16_t DeviceClock::MinuteOfDay(int64_t uptimeSecs) const
  {
    // UTC minute since 00:00 UTC, not since the 04:00 boundary. The app sends
    // the same, so neither side needs to know the boundary to compare minutes.
    return static_cast<uint16_t>((NowUnix(uptimeSecs) % M_SECONDS_PER_DAY) / M_SECONDS_PER_MINUTE);
  }

  bool DeviceClock::IsFresh(uint16_t presentedMinute, int64_t uptimeSecs) const
  {
    if (!m_valid) { return false; }
    return std::abs(MinuteDifference(presentedMinute, MinuteOfDay(uptimeSecs))) <= M_FRESHNESS_MINUTES;
  }

  DeviceClock::SyncResult DeviceClock::ApplyProvisionerSync(uint32_t unixSeconds, int64_t uptimeSecs)
  {
    uint16_t day { 0 };

    // Only an invalid clock accepts a sync. A valid clock is trimmed by commands
    // and never jumped: a captured sync released hours later would otherwise
    // pull the clock back and extend a day key past its 04:00 expiry.
    if (m_valid) { return SyncResult::AlreadyValid; }
    if (!DayIndexOf(unixSeconds, day)) { return SyncResult::BeforeEpoch; }

    // A first-ever boot has no floor, which is correct: that is the factory case.
    if (m_has_floor && day < m_floor_day) { return SyncResult::BelowFloor; }
    if (m_has_floor && day > m_floor_day + M_MAX_FORWARD_JUMP_DAYS) { return SyncResult::TooFarAhead; }

    m_offset_secs    = static_cast<int64_t>(unixSeconds) - uptimeSecs;
    m_valid          = true;
    m_trim_day       = day;
    m_trim_used_secs = 0;
    RaiseFloorDay(day);
    return SyncResult::Applied;
  }

  DeviceClock::TrimResult DeviceClock::ApplyMinuteHint(uint16_t presentedMinute, int64_t uptimeSecs)
  {
    int16_t differenceMinutes { 0 };
    int32_t deltaSecs { 0 };
    uint32_t magnitudeSecs { 0 };
    uint16_t today { 0 };
    uint16_t trimmedDay { 0 };

    if (!m_valid) { return TrimResult::ClockInvalid; }

    differenceMinutes = MinuteDifference(presentedMinute, MinuteOfDay(uptimeSecs));
    if (std::abs(differenceMinutes) < M_TRIM_DEADBAND_MINUTES) { return TrimResult::Unchanged; }
    if (std::abs(differenceMinutes) > M_TRIM_STEP_MAX_MINUTES) { return TrimResult::Ignored; }

    deltaSecs     = static_cast<int32_t>(differenceMinutes) * M_SECONDS_PER_MINUTE;
    magnitudeSecs = static_cast<uint32_t>(std::abs(deltaSecs));

    // The budget counts MAGNITUDE, not net movement, so alternating +5 and -5
    // minute hints cannot walk the clock around indefinitely.
    today = DayIndex(uptimeSecs);
    if (today != m_trim_day) {
      m_trim_day       = today;
      m_trim_used_secs = 0;
    }
    if (m_trim_used_secs + magnitudeSecs > M_TRIM_DAILY_BUDGET_SECS) { return TrimResult::OverBudget; }

    if (!DayIndexOf(static_cast<uint32_t>(uptimeSecs + m_offset_secs + deltaSecs), trimmedDay) || (m_has_floor && trimmedDay < m_floor_day)) {
      return TrimResult::WouldRewindDay;
    }

    m_offset_secs += deltaSecs;
    m_trim_used_secs += magnitudeSecs;
    return TrimResult::Trimmed;
  }

}
