#pragma once

#include <cstdint>

// Pure: no Zephyr headers, so the engine compiles and is tested on the host.
// Kconfig values reach it only through the CONFIG_ macros Zephyr pre-includes.
// The host fallback below applies ONLY when the host Makefile defines
// ALC_HOST_BUILD; any other build without the Kconfig value fails to compile,
// so the fallback can never silently apply on target.

namespace alc
{

#if defined(CONFIG_MFS_ADXL_INACTIVITY_SECS)
  constexpr uint32_t M_DETECTION_INACTIVITY_SECS { CONFIG_MFS_ADXL_INACTIVITY_SECS };
#elif defined(ALC_HOST_BUILD)
  // Host build only: the Kconfig default of MFS_ADXL_INACTIVITY_SECS.
  constexpr uint32_t M_DETECTION_INACTIVITY_SECS { 5 };
#else
#error "CONFIG_MFS_ADXL_INACTIVITY_SECS is not defined - the detection engine needs the Kconfig value on target."
#endif

  /**
   * @brief Everything the engine reports, one entry per log line App writes.
   *
   * Every event is delivered SYNCHRONOUSLY through
   * DetectionHardware::OnDetectionEvent(), at the exact point in the engine's
   * sequence where App logged the same thing before the extraction, so the
   * order of log lines against hardware calls is unchanged.
   *
   * The log LEVEL and WORDING for every event are owned entirely by
   * App::OnDetectionEvent() (src/app.cpp) - this engine only classifies what
   * happened and hands over the armed flag and any numeric fields. The
   * comments below are illustrative, not a contract: several events carry two
   * different wordings depending on `event.armed` or on App's own
   * m_logging_cooldown bookkeeping, and this header does not attempt to keep
   * every exact string in sync with App - read App::OnDetectionEvent for the
   * words actually logged today.
   *
   * The configure and AWAKE-read failures inside ConfigureAccelerometer() are
   * NOT events - the implementation logs them itself ("Accelerometer would not
   * configure: %d!", "Accelerometer AWAKE could not be read: %d!"), because only
   * it can tell the two apart.
   */
  enum class DetectionEventType : uint8_t {
    StaleAwakeSuppressed,      ///< WRN "ADXL still awake after configuring - suppressing until it clears!"
    StaleAwakeReleased,        ///< INF, worded by App per event.armed - "device is now live" vs "test is now live".
    Activation,                ///< INF "Activation %u of %u." - count, limit.
    CooldownStarted,           ///< INF "Cooldown started: %u s." - seconds.
    CooldownStandbyFailed,     ///< ERR, App appends its own retry wording - result.
    CooldownTimerFailed,       ///< ERR "Failed to start the cooldown timer: %d!" - result.
    CooldownTimerStopFailed,   ///< WRN "Could not stop the cooldown timer after a failure." - result.
    CooldownRestoreFailed,     ///< ERR, App appends its own retry wording - result.
    CooldownForced,            ///< WRN "Cooldown forced over by the deadline - the PMIC timer did not report expiry!"
    CooldownClearEventFailed,  ///< WRN "Failed to clear the cooldown timer expiry event!" - result.
    RearmFailed,               ///< ERR, worded by App per its own m_logging_cooldown - cooldown re-arm vs plain reconfigure.
    CooldownElapsed,           ///< INF, worded by App per its own m_logging_cooldown - cooldown elapsed vs reconfigured.
    DelayStarted,              ///< armed: WRN "TRIGGER PENDING: firing in %u s. Deactivating cancels it."; else a TEST line - seconds.
    DelayScanLost,             ///< ERR "Scanner not running at the start of a delay - a disarm may not be heard!" (armed only).
    DelayFastScanFailed,       ///< WRN "Continuous scan unavailable at the start of a delay - retrying; ..." (armed only) - result.
    DelayScanRestoreFailed,    ///< ERR "Failed to restore duty-cycled scanning: %d!" - result.
    DelayExpired,              ///< Armed: no log of its own. Disarmed: INF "TEST delay elapsed - LED B on."
    DelayExpiredScanLost,      ///< WRN "Trigger firing although the scanner was not running during the delay - ..." (armed only).
    WatchdogRearm,             ///< ERR "ADXL stuck AWAKE for %u s - re-arming the loop engine!" - seconds.
    WatchdogRearmFailed,       ///< ERR "ADXL re-arm failed!" - result.
    RestartCooldownStopFailed, ///< WRN or ERR, worded by App per event.armed - see App::OnDetectionEvent - result.
    RestartStandbyFailed,      ///< WRN, worded by App per event.armed - refused arm vs failed test restart.
  };

  /** @brief One engine event. Fields not named for the type are zero. */
  struct DetectionEvent
  {
      DetectionEventType type;
      bool armed;       ///< The armed flag of the Tick() or Restart() that raised it.
      uint16_t count;   ///< Activation: the count just reached.
      uint16_t limit;   ///< Activation: the configured activations.
      uint32_t seconds; ///< CooldownStarted, DelayStarted, WatchdogRearm.
      int32_t result;   ///< Failure events: the negative errno.
  };

  /** Hardware the engine drives. App implements it over Adxl367, Npm2100, k_timer and CommandScanner. */
  class DetectionHardware
  {
    public:
      virtual ~DetectionHardware() = default;

      /** Loop-mode bootstrap at the threshold, then read AWAKE from STATUS. Negative errno on failure. */
      virtual int ConfigureAccelerometer(uint16_t thresholdLsb, bool& awake) = 0;

      virtual int StandbyAccelerometer() = 0;

      /** Stop, set general-purpose mode, set duration, clear the event, start. Negative errno on failure. */
      virtual int StartCooldownTimer(uint32_t durationMs) = 0;

      virtual int StopCooldownTimer()                   = 0;
      virtual int CooldownTimerExpired(bool& expired)   = 0;
      virtual int ClearCooldownTimerEvent()             = 0;
      virtual void StartDelayTimer(uint32_t durationMs) = 0;
      virtual void StopDelayTimer()                     = 0;

      /** True while the GRTC delay timer still has time remaining. */
      virtual bool DelayTimerRunning() const = 0;

      /**
       * Continuous scan + PM lock while true. The engine requests true ONLY for
       * an armed delay; false is requested on every delay cancel, armed or not,
       * exactly as App::cancelDelay() did (a no-op when already duty-cycled).
       */
      virtual int SetTriggerPendingScan(bool fast) = 0;

      virtual bool ScannerRunning() const = 0;

      /**
       * The engine's ONLY reporting channel - see DetectionEventType. Called
       * synchronously from inside Restart() and Tick(). Must not call back into
       * the engine.
       */
      virtual void OnDetectionEvent(const DetectionEvent& event) = 0;
  };

  struct DetectionSettings
  {
      uint8_t activations;
      uint16_t cooldownSeconds;
      uint16_t delaySeconds;
      uint16_t thresholdLsb;
  };

  /**
   * @brief The detection engine: activation counting, cooldown, the delay before
   * triggering, the detection period and the stuck-AWAKE watchdog.
   *
   * Runs identically armed and disarmed (disarmed test mode amendment, section 2).
   * The only state-dependent behaviour inside it is that an ARMED delay requests
   * fast scan and tracks scanner loss, and that only an ARMED output completion
   * is reported by TakeTriggerComplete().
   *
   * It does NOT derive the output. App::updateOutputState() remains the single
   * derivation point - armed && DetectionMet() && DelayPermitsFiring() - and
   * reports what it derived through NoteOutput().
   */
  class DetectionEngine
  {
    public:
      /** Loop ticks per second - Tick() is called every 100 ms. */
      static constexpr uint32_t M_TICKS_PER_SECOND { 10 };

      // Stuck-AWAKE watchdog threshold, in 100 ms loop ticks. Generous multiple of
      // the configured inactivity period so normal sustained handling never trips it.
      static constexpr uint32_t M_AWAKE_STUCK_MULTIPLE { 6 };
      static constexpr uint32_t M_AWAKE_STUCK_TICKS { M_DETECTION_INACTIVITY_SECS * M_TICKS_PER_SECOND * M_AWAKE_STUCK_MULTIPLE };

      // How long a delayed trigger asserts: the ADXL loop period an undelayed
      // trigger gets from its own AWAKE.
      static constexpr int64_t M_MSEC_PER_SEC { 1000 };
      static constexpr int64_t M_DELAYED_TRIGGER_HOLD_MS { M_DETECTION_INACTIVITY_SECS * M_MSEC_PER_SEC };

      // nPM2100 TIMER is specified to +-10%, so the cooldown deadline fallback must
      // allow that much slack over the requested duration before it can be trusted
      // to mean the PMIC has gone silent.
      static constexpr int64_t M_COOLDOWN_TOLERANCE_DIVISOR { 10 };

      // Extra fixed slack on top of the tolerance, covering scheduling jitter in the
      // 100 ms poll loop itself.
      static constexpr int64_t M_COOLDOWN_GRACE_MS { 2000 };

      // Minimum spacing between re-arm retries once the cooldown has expired but
      // the configure keeps failing. One attempt per second bounds the driver's
      // own error logging to 1 Hz instead of the 10 Hz poll rate.
      static constexpr int64_t M_COOLDOWN_RETRY_MS { 1000 };

      explicit DetectionEngine(DetectionHardware& hardware);

      /**
       * Restart from zero (amendment section 3). Returns the configure result; on
       * failure the part is stood down and the 1 Hz re-arm retry is pending.
       *
       * Discards the activation count, detection latch and hold, previous-AWAKE
       * witness, cooldown (PMIC timer stopped if one was running, expiry latch and
       * retry state), pending delay (timer stopped, scan restored), one-shot flags
       * and watchdog ticks; then reconfigures through the loop-mode bootstrap and
       * sets the stale-AWAKE suppression from the post-configure AWAKE read.
       */
      int Restart(const DetectionSettings& settings, bool armed, int64_t nowMs);

      /** One 100 ms loop tick. awake is INT1's logical level. */
      void Tick(const DetectionSettings& settings, bool armed, bool awake, int64_t nowMs);

      bool DetectionMet() const { return m_detection_met; }

      /**
       * @brief True only when BOTH witnesses agree no delay is running.
       *
       * A flag left set with a dead timer waits for the deadline rather than
       * firing early. A running timer with a cleared flag still blocks firing.
       * Both failure directions are safe, which is the whole reason for using two
       * witnesses of different kinds.
       */
      bool DelayPermitsFiring() const;

      bool DelayPending() const { return m_delay_pending; }

      /** A pending delay that started while armed - a real trigger, not a test. */
      bool DelayPendingArmed() const { return m_delay_pending && m_delay_armed; }

      uint8_t ActivationCount() const { return m_activation_count; }
      bool InCooldown() const { return m_in_cooldown; }
      bool IgnoringStaleAwake() const { return m_ignore_stale_trigger; }

      /** The output was asserted this session and has now ended. App latches Inactive when armed. Cleared by the read. */
      bool TakeTriggerComplete();

      /** App reports whether the output actually asserted this tick (armed one-shot tracking). */
      void NoteOutput(bool outputActive);

    private:
      // Configures the ADXL367 and proves it is reporting inactivity. Every
      // (re)configure starts with no inherited latch.
      int enableAccelerometer(const DetectionSettings& settings, bool armed);

      // Stands the ADXL367 down and starts the PMIC timer for the cooldown between
      // counted activations. No-op when the cooldown is zero. On any failure that
      // can leave the part unconfigured - a failed standby, or a failed timer
      // start whose restoring configure also fails - it hands over to the retry
      // path, so the device is never left deaf with nothing to bring it back.
      int beginCooldown(const DetectionSettings& settings, bool armed, int64_t nowMs);

      // Polls the PMIC timer; on expiry re-arms the ADXL367 through the full
      // bootstrap so the engine cannot inherit a level from the blanking window.
      void serviceCooldown(const DetectionSettings& settings, bool armed, int64_t nowMs);

      // Enters the retry path: the cooldown is treated as already expired, so
      // serviceCooldown() only retries the configure.
      void handOverToRetry(int64_t firstRetryMs, bool failureLogged);

      void beginDelay(const DetectionSettings& settings, bool armed, int64_t nowMs);
      void cancelDelay(bool armed);

      void report(DetectionEventType type, bool armed, int32_t result = 0);
      void report(const DetectionEvent& event);

      DetectionHardware& m_hardware;

      // Latched when the activation count reaches the configured threshold (or a
      // delay expires); cleared when AWAKE de-asserts and any hold has passed.
      bool m_detection_met;

      // Activations seen since the last detection or restart. Does not expire.
      uint8_t m_activation_count;

      // Previous INT1 level, for edge detection. The engine counts RISING edges,
      // not levels - a level would count the same activation on every loop tick.
      bool m_previous_awake;

      // True when the ADXL367 was still awake immediately after being configured.
      // That assertion belongs to motion from BEFORE the configure, so it must not
      // count; it is suppressed until INT1 de-asserts and a fresh edge arrives.
      bool m_ignore_stale_trigger;

      // Consecutive loop ticks with the ADXL awake, for the stuck-AWAKE watchdog.
      uint32_t m_awake_ticks;

      // True while the ADXL is standing down for a cooldown window, or while a
      // failed configure is being retried.
      bool m_in_cooldown;

      // Uptime at which the cooldown is forced over regardless of what the PMIC
      // reports, so a fault or a timer that never expires cannot strand the
      // device in standby forever.
      int64_t m_cooldown_deadline_ms;

      // True once a failed re-arm has been reported, so a retry that keeps failing
      // reports once rather than every attempt.
      bool m_cooldown_rearm_failed;

      // The cooldown window itself is over and only the re-arm remains. Once set,
      // serviceCooldown() stops consulting the PMIC.
      bool m_cooldown_expired;

      // Earliest uptime at which the next re-arm attempt may run.
      int64_t m_cooldown_next_retry_ms;

      // The delay's flag witness; the timer is the other - see DelayPermitsFiring().
      bool m_delay_pending;

      // The pending delay started while armed: fast scan was requested for it and
      // scanner loss is tracked. A disarmed test delay has neither.
      bool m_delay_armed;

      // Uptime at which a pending delay is considered genuinely expired. A timer
      // reading "not running" is true both when it has expired and when it was
      // never started, so this deadline is what tells the two apart.
      int64_t m_delay_deadline_ms;

      // Set if the scanner was not running at the start of an ARMED delay, or on
      // any tick thereafter while it is pending. Does not suppress the trigger -
      // Andy's ruling is to prioritise the alarm - but is reported at expiry.
      bool m_delay_scan_lost;

      // Uptime until which a DELAYED detection holds regardless of AWAKE. Zero for
      // an undelayed detection, whose own AWAKE sets the duration.
      int64_t m_detection_hold_until_ms;

      // ARMED one-shot: set while the output is asserted (NoteOutput); when it
      // clears, the trigger is complete.
      bool m_trigger_fired;
      bool m_trigger_complete;

      // DISARMED counterpart of m_trigger_fired: a test detection period is in
      // progress, which blocks counting exactly as an armed trigger does. Its end
      // requests no latch.
      bool m_test_detection_active;

      // The armed flag of the most recent Tick(), or of a Restart() whose configure
      // succeeded (a refused arm leaves it false), for NoteOutput().
      bool m_armed;
  };

}
