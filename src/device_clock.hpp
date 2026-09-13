#pragma once

#include <cstdint>

namespace alc
{

  /**
   * @brief UTC wall time for the access scheme. A SECURITY COMPONENT.
   *
   * The day boundary this defines is what makes a lost day key expire, so every
   * rule here exists to stop the day index being moved by anyone but the trusted
   * provisioner, and never backwards. See docs/tan-scheme.md section 4 and
   * docs/power-budget.md section 8.7.3.
   *
   * Time is `uptime + offset`. Uptime is the GRTC clocked from the LFXO, passed in
   * by the caller in whole seconds so every rule is host-testable.
   *
   * **The clock starts INVALID on every boot** and becomes valid only through an
   * authenticated provisioner sync. There is no resume-from-NVS path: a device
   * that resumed on a persisted day would revive that day's keys, which is the
   * hole power-budget.md section 8.6 describes. The persisted value is used only
   * as a FLOOR that a sync may not go below.
   */
  class DeviceClock
  {
    public:
      // 2026-01-01T00:00:00Z.
      static constexpr uint32_t M_EPOCH_UNIX { 1767225600 };
      // Day boundary at 04:00 UTC - never local time. See tan-scheme.md section 4.
      static constexpr uint32_t M_DAY_BOUNDARY_OFFSET_SECS { 4 * 3600 };
      static constexpr uint32_t M_SECONDS_PER_DAY { 86400 };
      static constexpr uint16_t M_SECONDS_PER_MINUTE { 60 };
      static constexpr uint16_t M_MINUTES_PER_DAY { 1440 };

      // A sync more than this far past the floor is refused. Bounds the damage a
      // leaked provisioning key can do (pushing the clock years ahead is a denial
      // of service). A device stored unpowered for longer needs wired recovery.
      static constexpr uint16_t M_MAX_FORWARD_JUMP_DAYS { 400 };

      // A command whose minute is further than this from the device's own is
      // stale: it was captured, held and released. Rejected outright.
      static constexpr uint8_t M_FRESHNESS_MINUTES { 10 };
      // Differences below this are noise: 1-minute resolution and an advert up to
      // 30 s old when heard.
      static constexpr uint8_t M_TRIM_DEADBAND_MINUTES { 2 };
      // Differences above this, but still fresh, are ignored rather than clamped.
      static constexpr uint8_t M_TRIM_STEP_MAX_MINUTES { 5 };
      // Total trim magnitude permitted per day index. A correctly loaded LFXO
      // drifts ~2 s/day, so this is ~150x margin, and it caps a time-shift attack.
      static constexpr uint16_t M_TRIM_DAILY_BUDGET_SECS { 300 };

      enum class SyncResult : uint8_t {
        Applied,      ///< The clock is now valid.
        AlreadyValid, ///< Refused: syncs are accepted only while the clock is invalid.
        BeforeEpoch,  ///< Refused: earlier than the first 04:00 UTC of 2026.
        BelowFloor,   ///< Refused: would put the day before one already seen.
        TooFarAhead,  ///< Refused: more than M_MAX_FORWARD_JUMP_DAYS past the floor.
      };

      enum class TrimResult : uint8_t {
        Unchanged,      ///< Inside the deadband.
        Trimmed,        ///< Offset adjusted.
        Ignored,        ///< Fresh but beyond the step limit - the time field is not trusted.
        OverBudget,     ///< Would exceed today's trim budget.
        WouldRewindDay, ///< Would move the day index below the floor.
        ClockInvalid,
      };

      DeviceClock();

      /** @brief Day index of a UNIX time. @return False before the epoch's first boundary. */
      static bool DayIndexOf(uint32_t unixSeconds, uint16_t& day);

      /** @brief Signed minute difference presented - own, wrapped into [-720, 720). */
      static int16_t MinuteDifference(uint16_t presentedMinute, uint16_t ownMinute);

      /** @brief Raise the floor. Never lowers it. */
      void RaiseFloorDay(uint16_t day);

      bool HasFloor() const { return m_has_floor; }
      uint16_t FloorDay() const { return m_floor_day; }
      bool IsValid() const { return m_valid; }

      uint32_t NowUnix(int64_t uptimeSecs) const;

      /** @brief The current day index, never below the floor. Only meaningful when valid. */
      uint16_t DayIndex(int64_t uptimeSecs) const;

      /** @brief UTC minute of day, 0-1439. Only meaningful when valid. */
      uint16_t MinuteOfDay(int64_t uptimeSecs) const;

      /** @brief Whether a presented minute is within M_FRESHNESS_MINUTES. False while invalid. */
      bool IsFresh(uint16_t presentedMinute, int64_t uptimeSecs) const;

      /** @brief Apply an authenticated provisioner time. The tag must already have been verified. */
      SyncResult ApplyProvisionerSync(uint32_t unixSeconds, int64_t uptimeSecs);

      /** @brief Trim drift from an authenticated, fresh command's minute field. */
      TrimResult ApplyMinuteHint(uint16_t presentedMinute, int64_t uptimeSecs);

    private:
      bool m_valid;
      int64_t m_offset_secs;
      bool m_has_floor;
      uint16_t m_floor_day;
      uint16_t m_trim_day;
      uint32_t m_trim_used_secs;
  };

}
