#include "detection_engine.hpp"

// No LOG_MODULE_REGISTER: the engine is pure and reports through
// DetectionHardware::OnDetectionEvent() - see DetectionEventType.

namespace alc
{

  DetectionEngine::DetectionEngine(DetectionHardware& hardware)
      : m_hardware(hardware)
      , m_detection_met(false)
      , m_activation_count(0)
      , m_previous_awake(false)
      , m_ignore_stale_trigger(false)
      , m_awake_ticks(0)
      , m_in_cooldown(false)
      , m_cooldown_deadline_ms(0)
      , m_cooldown_rearm_failed(false)
      , m_cooldown_expired(false)
      , m_cooldown_next_retry_ms(0)
      , m_delay_pending(false)
      , m_delay_armed(false)
      , m_delay_deadline_ms(0)
      , m_delay_scan_lost(false)
      , m_detection_hold_until_ms(0)
      , m_trigger_fired(false)
      , m_trigger_complete(false)
      , m_test_detection_active(false)
      , m_armed(false)
  {}

  int DetectionEngine::Restart(const DetectionSettings& settings, bool armed, int64_t nowMs)
  {
    // ================================================================
    //  RESTART FROM ZERO - SAFETY CRITICAL. See the disarmed test mode
    //  amendment, sections 3 and 3.2.
    //
    //  Disarm, Settings, a completed armed trigger and ARMING all come
    //  through here. Nothing the previous session held survives: an
    //  engineer may arm at any moment of a test, and no count, cooldown,
    //  delay, detection or AWAKE level from that test may reach the
    //  fire output of the armed device.
    // ================================================================
    int result { 0 };
    int standbyResult { 0 };

    // Tagged armed only once the configure has succeeded, below - a refused arm
    // must not drive NoteOutput()'s armed one-shot. Events raised during the
    // restart still carry the REQUESTED armed flag, so App can word them.
    m_armed = false;

    // A pending delay - armed trigger or test - never outlives a restart. The
    // pending state is also deliberately not persisted, so a reset loses it too
    // - the fail-safe direction.
    cancelDelay(armed);
    m_delay_scan_lost   = false;
    m_delay_deadline_ms = 0;

    m_activation_count        = 0;
    m_detection_met           = false;
    m_detection_hold_until_ms = 0;
    m_previous_awake          = false;
    m_ignore_stale_trigger    = false;
    m_awake_ticks             = 0;

    // A cooldown - or its expiry latch, or a pending re-arm retry - from before
    // this restart must not carry into the new session.
    if (m_in_cooldown) {
      result = m_hardware.StopCooldownTimer();
      if (result < 0) { report(DetectionEventType::RestartCooldownStopFailed, armed, result); }
    }
    m_in_cooldown            = false;
    m_cooldown_expired       = false;
    m_cooldown_rearm_failed  = false;
    m_cooldown_next_retry_ms = 0;
    m_cooldown_deadline_ms   = 0;

    // Whatever the route here, a trigger or test detection in progress is over.
    m_trigger_fired         = false;
    m_trigger_complete      = false;
    m_test_detection_active = false;

    // EDGE-TRIGGERED ARMING. The ADXL367 AWAKE bit is a LEVEL, not a latch, so a
    // part left running would hand an assertion from before this restart - very
    // often the engineer's own handling of the device - straight to the new
    // session. Configuring IS the clear: the loop-mode bootstrap soft-resets the
    // part and forces one activity/inactivity cycle, and enableAccelerometer()
    // suppresses any AWAKE still reported after it.
    result = enableAccelerometer(settings, armed);
    if (result < 0) {
      // Best effort - the part must not keep running unconfigured. The retry path
      // then keeps trying at 1 Hz so the test resumes as soon as the part
      // responds; the caller has already been told through the return value, so
      // the first failed retry is not reported again.
      standbyResult = m_hardware.StandbyAccelerometer();
      if (standbyResult < 0) { report(DetectionEventType::RestartStandbyFailed, armed, standbyResult); }
      handOverToRetry(nowMs + M_COOLDOWN_RETRY_MS, true);
      return result;
    }

    m_armed = armed;
    return 0;
  }

  void DetectionEngine::Tick(const DetectionSettings& settings, bool armed, bool awake, int64_t nowMs)
  {
    bool risingEdge { false };
    bool delayWasArmed { false };

    m_armed = armed;

    // Every tick, not throttled (formerly App::serviceScanHealth()): an ARMED
    // delay that goes blind to the scanner must not complete as if nothing
    // happened. Keyed on "running" alone - a scan at the wrong cadence can still
    // hear a disarm, just more slowly. A test delay has nothing to disarm.
    if (m_delay_pending && m_delay_armed && !m_hardware.ScannerRunning()) { m_delay_scan_lost = true; }

    serviceCooldown(settings, armed, nowMs);

    // Release the stale-trigger suppression only once the part has actually gone
    // back to sleep. This is what makes arming edge-triggered.
    if (m_ignore_stale_trigger && !awake) {
      m_ignore_stale_trigger = false;
      report(DetectionEventType::StaleAwakeReleased, armed);
    }

    // RISING EDGES, not levels. AWAKE stays asserted for the whole inactivity
    // period, so counting the level would add one activation per loop tick.
    // While a trigger's output period - or a test's detection period - is in
    // progress no new activation is counted, so a second delay cannot start and
    // cut the one-shot pulse short.
    risingEdge =
        awake && !m_previous_awake && !m_ignore_stale_trigger && !m_in_cooldown && !m_delay_pending && !m_trigger_fired && !m_test_detection_active;
    m_previous_awake = awake;

    if (risingEdge) {
      DetectionEvent activation { DetectionEventType::Activation, armed, 0, settings.activations, 0, 0 };

      m_activation_count++;
      activation.count = m_activation_count;
      report(activation);

      if (m_activation_count >= settings.activations) {
        m_activation_count = 0;
        // No blanking here. Standing the ADXL down at the moment of detection
        // would cut short the assertion that IS the output's 5 s duration.
        //
        // The delay runs in BOTH states (amendment section 2); only an armed
        // delay requests fast scan and the PM lock - see beginDelay().
        if (settings.delaySeconds > 0) {
          beginDelay(settings, armed, nowMs); // m_detection_met waits for the timer
        } else {
          m_detection_met = true;
        }
      } else {
        // Result not checked here - beginCooldown() already reports its own
        // failure, and on failure it has itself restored detection or handed
        // over to the retry path.
        beginCooldown(settings, armed, nowMs);
      }
    }

    // The delay elapsed and was not cancelled. The timer alone cannot be trusted
    // here: it reads "not running" both for "expired" and for "never started",
    // and m_delay_deadline_ms is what tells the two apart.
    if (m_delay_pending && !m_hardware.DelayTimerRunning() && nowMs >= m_delay_deadline_ms) {
      delayWasArmed = m_delay_armed;
      cancelDelay(armed); // clears the flag and restores duty-cycled scanning
      m_detection_met = true;

      // The delay outlived the AWAKE that started it, so AWAKE is already clear
      // and would end detection on this very tick - the output would never
      // assert. Hold detection for the same 5 s the loop period gives an
      // undelayed trigger.
      m_detection_hold_until_ms = nowMs + M_DELAYED_TRIGGER_HOLD_MS;
      report(DetectionEventType::DelayExpired, armed);

      // Andy's ruling: the alarm is prioritised over the risk of a missed
      // disarm. A scanner outage during the delay does not suppress the
      // trigger - it is reported instead, so a missed disarm is at least
      // visible after the fact.
      if (delayWasArmed && m_delay_scan_lost) { report(DetectionEventType::DelayExpiredScanLost, armed); }
    }

    // The detection's own AWAKE running to completion is what clears it - or,
    // for a delayed detection, the hold set when the delay expired.
    if (m_detection_met && !awake && nowMs >= m_detection_hold_until_ms) { m_detection_met = false; }

    // Stuck-AWAKE watchdog. Defence in depth: if the accelerometer somehow holds
    // AWAKE far beyond its configured inactivity period, the device stops
    // triggering and - worse - does so SILENTLY, with no LED and no log. That is
    // an unacceptable failure mode for an alarm sensor, so recover rather than
    // sit dead. Re-running the loop configuration includes the bootstrap that
    // guarantees AWAKE clears.
    //
    // In BOTH states (amendment section 3.3): the part now runs in both.
    if (awake) {
      if (++m_awake_ticks >= M_AWAKE_STUCK_TICKS) {
        DetectionEvent stuck { DetectionEventType::WatchdogRearm, armed, 0, 0, M_AWAKE_STUCK_TICKS / M_TICKS_PER_SECOND, 0 };
        int result { 0 };

        m_awake_ticks = 0;
        report(stuck);

        // enableAccelerometer(), not a bare configure - the watchdog's re-arm
        // must reset the previous-awake edge witness and the stale-trigger hold
        // exactly like every other (re)configure, or a fresh motion edge right
        // after recovery could be lost or miscounted.
        //
        // enableAccelerometer() itself sets m_ignore_stale_trigger = awake on
        // success - if AWAKE is still asserted immediately after reconfiguring,
        // that assertion predates this recovery and must be suppressed until
        // INT1 de-asserts, exactly as for every other arm. DO NOT clear it again
        // below: an earlier version did, which discarded that suppression and
        // let a level still stuck right after the re-arm read as a rising edge
        // on the very next tick - counting a spurious activation once per
        // watchdog period until the configured count was reached and the device
        // fired on a stale level.
        result = enableAccelerometer(settings, armed);
        if (result < 0) {
          report(DetectionEventType::WatchdogRearmFailed, armed, result);

          // Hand over to serviceCooldown()'s existing re-arm retry path rather
          // than leave the part unconfigured and the watchdog silent until it
          // next trips M_AWAKE_STUCK_TICKS later - that would leave the device
          // deaf for the whole watchdog period again. The retry's own
          // enableAccelerometer() call sets m_ignore_stale_trigger correctly on
          // whichever retry eventually succeeds.
          handOverToRetry(0, false);
        }

        // A stuck level must not hold a detection - and so the output - open.
        m_detection_met = false;
      }
    } else {
      m_awake_ticks = 0;
    }

    // DISARMED one-shot counterpart. The armed period is tracked from the output
    // App actually derived - see NoteOutput(). A test has no output, so its
    // period is the condition the output would have had; its end requests no
    // latch (amendment section 3: the test carries on).
    if (!armed) { m_test_detection_active = m_detection_met && DelayPermitsFiring(); }
  }

  bool DetectionEngine::DelayPermitsFiring() const
  {
    // BOTH must agree. Not one, not either - both.
    return !m_delay_pending && !m_hardware.DelayTimerRunning();
  }

  bool DetectionEngine::TakeTriggerComplete()
  {
    bool complete { m_trigger_complete };

    m_trigger_complete = false;
    return complete;
  }

  void DetectionEngine::NoteOutput(bool outputActive)
  {
    // ONE-SHOT. Once the output has asserted and its period has ended, the
    // trigger is complete. Only flagged here - App acts on it in the main loop.
    // Output can only be active while armed; a disarmed tick has nothing to note.
    if (!m_armed) { return; }

    if (outputActive) { m_trigger_fired = true; }
    if (m_trigger_fired && !outputActive) {
      m_trigger_fired    = false;
      m_trigger_complete = true;
    }
  }

  int DetectionEngine::enableAccelerometer(const DetectionSettings& settings, bool armed)
  {
    // ================================================================
    //  ENABLE ORDER - SAFETY CRITICAL. See docs/v1-scope.md section 1.0.1.
    //
    //  Configure the part and prove it is reporting inactivity. App sets
    //  its arm boolean Active only after Restart() returns success, so
    //  the device cannot come up armed on motion that predates arming.
    // ================================================================
    int result { 0 };
    bool awake { true };

    // Configuring IS the clear: the datasheet's loop mode initialization routine
    // soft-resets the part and forces one activity/inactivity cycle, which drives
    // AWAKE low and captures a valid reference. AWAKE is confirmed from STATUS
    // rather than from INT1 - the register is what the engine actually holds.
    result = m_hardware.ConfigureAccelerometer(settings.thresholdLsb, awake);
    if (result < 0) { return result; }

    // Every (re)configure starts with no inherited latch - a detection from
    // before this configure must never reach the output (v1-scope section 1.0.1).
    m_detection_met           = false;
    m_previous_awake          = false;
    m_detection_hold_until_ms = 0;

    // Should already be clear. If handling the device has woken it again in the
    // moments since, that assertion still predates the configure, so suppress it
    // until INT1 de-asserts and a fresh edge arrives.
    m_ignore_stale_trigger = awake;
    if (awake) { report(DetectionEventType::StaleAwakeSuppressed, armed); }

    m_awake_ticks = 0;
    return 0;
  }

  int DetectionEngine::beginCooldown(const DetectionSettings& settings, bool armed, int64_t nowMs)
  {
    int64_t durationMs { static_cast<int64_t>(settings.cooldownSeconds) * M_MSEC_PER_SEC };
    int result { 0 };
    int cleanupResult { 0 };

    if (settings.cooldownSeconds == 0) { return 0; }

    // Stand the accelerometer down for the window. Leaving it running would let
    // a continuous disturbance hold AWAKE asserted right through the blanking
    // period, so the re-arm would inherit a stale level - exactly the bug commit
    // 0a50910 fixed for the arming path.
    result = m_hardware.StandbyAccelerometer();
    if (result < 0) {
      report(DetectionEventType::CooldownStandbyFailed, armed, result);

      // NEVER just return. Standby is several register writes, and INT1 is
      // unmapped before POWER_CTL is written - a failure part-way leaves the part
      // deaf with INT1 silent, which the stuck-AWAKE watchdog cannot see. Hand
      // over to the retry path so the full bootstrap runs within
      // M_COOLDOWN_RETRY_MS and keeps running at 1 Hz until detection is back.
      handOverToRetry(nowMs + M_COOLDOWN_RETRY_MS, true);
      return result;
    }

    result = m_hardware.StartCooldownTimer(static_cast<uint32_t>(durationMs));
    if (result < 0) {
      report(DetectionEventType::CooldownTimerFailed, armed, result);

      // Fail TOWARD detecting - no blanking this time - rather than leave the
      // part standing down with nothing left to bring it back up.
      cleanupResult = m_hardware.StopCooldownTimer();
      if (cleanupResult < 0) { report(DetectionEventType::CooldownTimerStopFailed, armed, cleanupResult); }
      cleanupResult = enableAccelerometer(settings, armed);
      if (cleanupResult < 0) {
        report(DetectionEventType::CooldownRestoreFailed, armed, cleanupResult);

        // The part is stood down and would not configure. Without a hand-over
        // nothing would ever bring it back: not in cooldown, so no retry, and
        // INT1 silent, so no watchdog - a SILENT loss of the alarm. The retry
        // path treats the window as already expired, so the PMIC timer that
        // failed to start is never consulted.
        handOverToRetry(nowMs + M_COOLDOWN_RETRY_MS, true);
      }
      return result;
    }

    m_in_cooldown            = true;
    m_cooldown_rearm_failed  = false;
    m_cooldown_expired       = false;
    m_cooldown_next_retry_ms = 0;

    // Forced over regardless of the PMIC - see m_cooldown_deadline_ms. The TIMER
    // block is +-10%, plus a fixed grace for loop scheduling jitter.
    m_cooldown_deadline_ms = nowMs + durationMs + durationMs / M_COOLDOWN_TOLERANCE_DIVISOR + M_COOLDOWN_GRACE_MS;

    report(DetectionEvent { DetectionEventType::CooldownStarted, armed, 0, 0, settings.cooldownSeconds, 0 });
    return 0;
  }

  void DetectionEngine::serviceCooldown(const DetectionSettings& settings, bool armed, int64_t nowMs)
  {
    bool expired { false };
    bool pmicExpired { false };
    int result { 0 };

    if (!m_in_cooldown) { return; }

    // Only consult the PMIC until the cooldown is confirmed over. Once
    // m_cooldown_expired is set the window itself has already ended - the timer's
    // expiry event is already cleared - and everything left is the re-arm retry,
    // which has nothing to do with the PMIC timer.
    if (!m_cooldown_expired) {
      // A CooldownTimerExpired() error counts as not-expired from the PMIC, but
      // the deadline below still applies - it is the fallback for exactly this case.
      pmicExpired = (m_hardware.CooldownTimerExpired(expired) == 0) && expired;
      expired     = pmicExpired || (nowMs >= m_cooldown_deadline_ms);
      if (!expired) { return; }

      // Reported only on this transition, not on every later retry tick.
      if (!pmicExpired) { report(DetectionEventType::CooldownForced, armed); }
      result = m_hardware.ClearCooldownTimerEvent();
      if (result < 0) { report(DetectionEventType::CooldownClearEventFailed, armed, result); }

      m_cooldown_expired = true;
    }

    // Rate-limited: one attempt per M_COOLDOWN_RETRY_MS rather than every 100 ms
    // poll tick, so a persistently failing re-arm cannot flood RTT via the
    // driver's own logging.
    if (nowMs < m_cooldown_next_retry_ms) { return; }

    // Full bootstrap, not a bare restart. Re-arming must confirm AWAKE is clear
    // so the engine cannot inherit an assertion from during the blanking window.
    // m_in_cooldown is left set on failure so the re-arm is retried, rather than
    // stranding the ADXL in standby forever.
    result = enableAccelerometer(settings, armed);
    if (result < 0) {
      if (!m_cooldown_rearm_failed) {
        report(DetectionEventType::RearmFailed, armed, result);
        m_cooldown_rearm_failed = true;
      }
      m_cooldown_next_retry_ms = nowMs + M_COOLDOWN_RETRY_MS;
      return;
    }

    m_in_cooldown      = false;
    m_cooldown_expired = false;
    m_previous_awake   = false;
    report(DetectionEventType::CooldownElapsed, armed);
  }

  void DetectionEngine::handOverToRetry(int64_t firstRetryMs, bool failureLogged)
  {
    m_in_cooldown            = true;
    m_cooldown_expired       = true;
    m_cooldown_next_retry_ms = firstRetryMs;
    m_cooldown_rearm_failed  = failureLogged;
  }

  void DetectionEngine::beginDelay(const DetectionSettings& settings, bool armed, int64_t nowMs)
  {
    int64_t durationMs { static_cast<int64_t>(settings.delaySeconds) * M_MSEC_PER_SEC };
    int result { 0 };

    if (settings.delaySeconds == 0) { return; }

    // GRTC, not the PMIC timer. At +/-10% over temperature the PMIC would put a
    // 9-hour delay anywhere inside a 108-minute window.
    m_hardware.StartDelayTimer(static_cast<uint32_t>(durationMs));
    m_delay_pending     = true;
    m_delay_armed       = armed;
    m_delay_deadline_ms = nowMs + durationMs;
    m_delay_scan_lost   = false;

    // ARMED ONLY: stay awake and scan continuously. The deactivate path is the
    // most important thing the device does while a trigger is pending, and at
    // the duty-cycled 5906 ms cadence an abort takes ~30 s to be heard with
    // confidence. A disarmed test delay has nothing dangerous pending, so a
    // 9 h test costs no extra battery (amendment section 2).
    if (armed) {
      result = m_hardware.SetTriggerPendingScan(true);
      if (!m_hardware.ScannerRunning()) {
        // Truly down - nothing is listening at all.
        m_delay_scan_lost = true;
        report(DetectionEventType::DelayScanLost, armed);
      } else if (result < 0) {
        // Still running, just at the fallback (duty-cycled) cadence - a disarm
        // can still be heard, only more slowly. Not scan-lost: App's scan health
        // service keeps retrying for the true continuous cadence.
        report(DetectionEventType::DelayFastScanFailed, armed, result);
      }
    }

    report(DetectionEvent { DetectionEventType::DelayStarted, armed, 0, 0, settings.delaySeconds, 0 });
  }

  void DetectionEngine::cancelDelay(bool armed)
  {
    int result { 0 };

    m_hardware.StopDelayTimer();
    m_delay_pending = false;
    m_delay_armed   = false;

    // Unconditional, as before the extraction: releases the PM lock if held and
    // restores duty-cycled scanning, a no-op when already duty-cycled.
    result = m_hardware.SetTriggerPendingScan(false);
    if (result < 0) {
      // App's scan health service keeps retrying - a failed cadence change here
      // must not be silently lost.
      report(DetectionEventType::DelayScanRestoreFailed, armed, result);
    }
  }

  void DetectionEngine::report(DetectionEventType type, bool armed, int32_t result)
  {
    report(DetectionEvent { type, armed, 0, 0, 0, result });
  }

  void DetectionEngine::report(const DetectionEvent& event)
  {
    m_hardware.OnDetectionEvent(event);
  }

}
