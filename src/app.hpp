#pragma once

#include <cstdint>

#include <zephyr/device.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/kernel.h>

#include "access_control.hpp"
#include "adxl367.hpp"
#include "arm_policy.hpp"
#include "arming_sequence.hpp"
#include "command_advertiser.hpp"
#include "command_scanner.hpp"
#include "detection_engine.hpp"
#include "device_clock.hpp"
#include "fire_sequence.hpp"
#include "led_indicator.hpp"
#include "led_sequencer.hpp"
#include "npm2100.hpp"
#include "output_switch.hpp"
#include "scan_policy.hpp"
#include "settings.hpp"

namespace alc
{

  /**
   * @brief Owns MFS_1 bring-up and the main loop.
   *
   * One Run() per boot. The SoC stays in System ON idle between scan windows —
   * the nPM2100 is never hibernated, so Run() does not return.
   *
   * Implements DetectionHardware privately: the detection engine drives the
   * ADXL367, the PMIC cooldown timer, the GRTC delay timer and the scanner
   * through it, and nothing outside App can.
   *
   * Implements ArmingActions privately: the arming sequence isolates and enables
   * the fire pins, restarts detection and raises the warning through it.
   */
  class App : private DetectionHardware, private ArmingActions
  {
    public:
      App();

      /** @brief Brings up the peripherals and enters the main loop. Does not return. */
      int Run();

      /**
       * @brief The device output state — the ONLY sanctioned trigger source.
       *
       * True only when the device is Active AND the ADXL367 is reporting motion
       * that began after arming. **The arm state (m_arming.State()) is
       * definitive**; the accelerometer is only ever ANDed with it. Arming is not
       * Active.
       *
       * Every consumer must use this. Nothing may read INT1, the AWAKE bit or the
       * ADXL367 directly and act on it: in the product this output switches a
       * voltage, so a device that fires while deactivated is dangerous. See
       * App::updateOutputState() and docs/v1-scope.md section 1.0.
       */
      bool IsOutputActive() const { return m_output_active; }

    private:
      /** @brief Whether updateOutputState() runs a detection engine tick first. */
      enum class EngineTick : uint8_t { Run, Skip };

      /** @brief Where logArmingFailure() is called from - selects the ScannerCheck wording. */
      enum class FailureSource : uint8_t { ArmCommand, Sequence };

      int initLeds();

      // Probes the PMIC, reports the reset reason, disables the SHPHLD power-off
      // path, and brings up LSOUT at 1.8 V in Ultra-Low Power mode for the ADXL367.
      int initPmic();

      // Drops LDOSW to Ultra-Low Power once the ADXL367 has been probed and
      // parked in standby - the loop-mode configure that actually needs power
      // happens afterwards, when the boot disarm starts the detection test. The
      // rail is brought up in High Power because the ADXL367 needs >250 uA
      // during power-up for correct fuse loading.
      int lowerLsoutToUlp();

      int initAccelerometer();

      // Writes all three LEDs. Called by the main loop only while no overlay
      // plays - the LED timer owns every pin while one does. See overlayActive in Run().
      int applyLeds(const LedLevels& levels);

      // Derives m_output_active. The single place the arm state and the
      // accelerometer are combined — see IsOutputActive(). EngineTick::Skip
      // re-derives without advancing the engine, for the disarm path.
      void updateOutputState(EngineTick tick);

      // Every disarm path - command, trigger latch, boot - comes through here:
      // ArmingSequence::Disarm() (pins disabled FIRST, then Inactive, then
      // RestartDetection(false)), then any failure it recorded is logged.
      // Returns whether arming was cancelled.
      bool disarmDevice();

      // Logs a failure ArmingSequence recorded, if any. The warning was already
      // raised by the sequence through SignalWarning(). FailureSource::ArmCommand
      // selects the refusal wording for a ScannerCheck failure raised by
      // BeginArming().
      void logArmingFailure(FailureSource source);

      // The warning (arming sequence amendment section 4). Logs, and plays orange
      // steady for 3 s in every build, outranking any other overlay (three-LED
      // spec section 3.2). Must not call back into m_arming.
      void signalWarning(const char* reason, int result);

      // Raises the fire supply, waits M_FIRE_SUPPLY_SETTLE_MS and verifies VOUT.
      // Shared by the arming battery test and prepareFireSupply(). 0 or negative
      // errno. Sets m_fire_supply_raised once the boost write is attempted.
      int raiseFireSupply();

      // Fire supply amendment (2026-11-14): armed, the boost idles at its VSET
      // level and is raised only when a fire is pending - a live FIRE accepted, an
      // armed trigger delay started, or, as the backstop, the output about to
      // assert. At most one attempt per pending fire. A failed raise is logged and
      // the fire goes ahead (owner decision): aborting is no safer.
      void prepareFireSupply();

      // ArmingActions - see arming_sequence.hpp for each contract.
      //
      // NONE of these may call back into m_arming, directly or indirectly.
      // Commands are handled only from the main loop, in serviceCandidates(), and
      // nothing these call services the scanner queue.
      int DisableFirePins() override;
      int RestartDetection(bool armed) override;
      int EnableFirePins() override;
      int RaiseFireSupply() override;
      void LowerFireSupply() override;
      // ScannerRunning() is declared once, below with DetectionHardware, and
      // overrides both interfaces.
      bool TakeScannerLost() override;
      void SignalWarning(ArmingStep step, int result) override;

      // The engine's settings, built from m_settings on every call - never cached,
      // so a Settings command reaches the engine on its very next use.
      DetectionSettings detectionSettings() const;

      // Restarts the detection engine from zero at the current settings. Returns
      // the configure result - see DetectionEngine::Restart().
      int restartEngine(bool armed);

      // Delegates to DetectionEngine::DelayPermitsFiring() - both delay witnesses
      // must agree no delay is running.
      bool delayPermitsFiring() const;

      // Installed into OutputSwitch as the second, independent layer.
      static bool interlockThunk(void* context);

      // Confirms the scanner is at its requested cadence and retries at
      // M_SCAN_SERVICE_INTERVAL_MS if not. It does not act on scanner loss while
      // arming or armed: Run() fails safe through
      // ArmingSequence::ServiceScannerHealth(), which also sees a loss this retry
      // has already healed (CommandScanner::TakeScanLost()), and the detection
      // engine suppresses an armed delay that lost its scanner. The retry can
      // itself leave the scanner down, which fails safe the same way. Called
      // every main-loop tick.
      void serviceScanHealth();

      // THE ONE PLACE the scanner cadence is applied. Computes DesiredFastScan()
      // from the arm state and m_trigger_pending_scan and hands it to
      // CommandScanner::SetFastScan() (a no-op when already there and scanning).
      // Logs the reason when continuous scanning is newly requested. Returns the
      // scanner's result, which the engine reports as DelayFastScanFailed or
      // DelayScanRestoreFailed; a failure stays requested in the scanner and
      // serviceScanHealth() retries it.
      //
      // Called from SetTriggerPendingScan(), after a successful BeginArming(), and
      // from the main loop only when the desired cadence differs from the one last
      // requested - never directly from an ArmingActions implementation. It IS
      // reached indirectly from one: RestartDetection() restarts the detection
      // engine, whose cancelDelay() calls SetTriggerPendingScan(false), which lands
      // here while the arming sequence is still inside its own call. That is safe
      // only because this function reads m_arming.State() and never calls into the
      // sequence. Keep it that way: a call into ArmingSequence from here would
      // re-enter it.
      int applyScanCadence();

      // DetectionHardware - see detection_engine.hpp for each contract.
      int ConfigureAccelerometer(uint16_t thresholdLsb, bool& awake) override;
      int StandbyAccelerometer() override;
      int StartCooldownTimer(uint32_t durationMs) override;
      int StopCooldownTimer() override;
      int CooldownTimerExpired(bool& expired) override;
      int ClearCooldownTimerEvent() override;
      void StartDelayTimer(uint32_t durationMs) override;
      void StopDelayTimer() override;
      bool DelayTimerRunning() const override;
      int SetTriggerPendingScan(bool fast) override;
      bool ScannerRunning() const override;
      void OnDetectionEvent(const DetectionEvent& event) override;

      // Parses the bench credentials from Kconfig, initialises PSA, runs the crypto
      // self-test and restores the access state. A failure leaves commands
      // disabled for the whole boot - see m_access_ready.
      int initAccess();

      // Drains the scanner queue. While the clock is invalid a candidate is offered
      // ONLY to the time-sync check; once valid, ONLY to AccessControl.
      void serviceCandidates();

#if defined(CONFIG_MFS_SCAN_DIAG)
      // Bench only: logs scanner reception counts once a second - see Kconfig.
      void serviceScanDiagnostics();
#endif

      void handleTimeSyncCandidate(const CommandScanner::Candidate& candidate, int64_t uptimeSecs);

      void handleCommandCandidate(const CommandScanner::Candidate& candidate, int64_t uptimeSecs);

      // Carries out an ACCEPTED command: arm transitions, settings, clock trim and
      // the confirmation overlay. Called only after AccessControl has persisted
      // the sequence number.
      void applyCommand(const AccessControl::Evaluation& evaluation, int64_t uptimeSecs);

      // Starts an overlay pattern and the 10 ms timer that renders it. A pattern
      // outranked by the one playing is dropped - see LedSequencer::Start().
      void playLedPattern(LedPattern pattern);

      // Stops a red flash still playing; any other overlay carries on. Called on
      // every disarm, so a disarmed device never shows the armed pattern.
      void cancelRedFlash();

      // Sets m_fault_display_latched: the device disarmed itself. Called only at
      // the armed-trigger, switch-fault and scanner-loss disarms in Run().
      void latchFaultDisplay();

      // k_timer expiry: renders all three LEDs while an overlay plays, then stops itself.
      static void ledTimerHandler(struct k_timer* timer);

      // Adopts the clock's current day at most once every M_ADVANCE_INTERVAL_SECS,
      // so a device that receives no commands for days still advances its
      // persisted floor and cannot later accept a stale captured provisioner sync.
      void serviceDayRollover();

      const struct device* m_i2c_bus;
      Npm2100 m_pmic;
      Adxl367 m_accelerometer;
      CommandScanner m_scanner;

      // UTC for the access scheme. Invalid on every boot until a provisioner sync.
      DeviceClock m_clock;

      // The only judge of whether a candidate is an authentic, fresh command.
      AccessControl m_access;

      // Uptime of the last serviceDayRollover() call that attempted Advance().
      // See M_ADVANCE_INTERVAL_SECS.
      int64_t m_last_advance_secs;

      // False if credentials, PSA or the self-test failed. Commands and syncs are
      // then ignored for the whole boot: a backend that disagrees with the app
      // must not be trusted to judge anything.
      bool m_access_ready;

      // The device's actual output. Driven ONLY from updateOutputState(), which
      // is the single derivation point - see IsOutputActive(). This class owns
      // its pins privately; nothing else can reach them.
      OutputSwitch m_output_switch;

      // The definitive output state. Written only by updateOutputState(), read
      // only via IsOutputActive().
      bool m_output_active;

      // Set by updateOutputState() when a fire switch failure while Active raises
      // the warning; taken by the main loop, which fails safe to disarmed. Warned
      // at most once per Active session, and cleared when a new session starts.
      // A later refused arm (the latched switch fails Enable()) is warned by the
      // arming sequence instead - see Run().
      bool m_switch_fault_pending;

      // The fire supply was raised - or a raise attempted - since the last
      // LowerFireSupply(). Stops prepareFireSupply() repeating the 100 ms settle.
      bool m_fire_supply_raised;

      // The engineer-settable parameters, NVS-backed.
      Settings m_settings;

      // The GRTC delay timer - one of the delay's two witnesses, the engine's flag
      // being the other. See DetectionEngine::DelayPermitsFiring().
      struct k_timer m_delay_timer;

      // Held while an ARMED delay is pending so the SoC cannot enter a deeper
      // state - only takes effect when CONFIG_PM is enabled (it is off in this
      // build, so continuous scanning is the mechanism that actually keeps a
      // disarm heard promptly). Owned by SetTriggerPendingScan().
      bool m_delay_pm_lock_held;

      // The detection engine's request for continuous scanning during an ARMED
      // trigger delay - one input to DesiredFastScan(), the arm state being the
      // other. Written only by SetTriggerPendingScan().
      bool m_trigger_pending_scan;

      // Uptime of the last scanner health check - see serviceScanHealth().
      int64_t m_last_scan_service_ms;

      // See serviceScanDiagnostics(). Unused unless CONFIG_MFS_SCAN_DIAG.
      int64_t m_last_scan_diag_ms;

      // True once a scan outage has been logged, so a scanner that stays down
      // logs once rather than every M_SCAN_SERVICE_INTERVAL_MS tick. Cleared
      // once the scanner is confirmed running again.
      bool m_scan_outage_logged;

      // Logging only - the engine owns the cooldown. True between a
      // CooldownStarted event and the cooldown's end, so that a configure retry
      // (after a refused arm, a failed restart or a failed watchdog re-arm),
      // which the engine also runs as a "cooldown", is not logged as one.
      bool m_logging_cooldown;

      // The FIRE command's 10 s unstoppable countdown and bounded assertion
      // window. Serviced every main-loop tick; its live latch is ORed into the
      // output in updateOutputState() and its rehearsal latch lights red. It
      // self-terminates through the engine one-shot - no second disarm path.
      FireSequence m_fire;

      // The confirmation burst - a non-connectable advertising window opened on
      // FIRE receipt (both live and rehearsal), carrying confirmId as its
      // service UUID. Serviced every main-loop tick; see applyCommand().
      CommandAdvertiser m_advertiser;

      // Activation counting, cooldown, the delay, the detection period and the
      // stuck-AWAKE watchdog - in both arm states. Declared after everything its
      // DetectionHardware calls touch.
      DetectionEngine m_engine;

      // Inactive -> Arming -> Active, and every way back. The arm state lives
      // here and nowhere else. Declared after everything its ArmingActions calls
      // touch - the fire output, the engine and the output state.
      ArmingSequence m_arming;

      // The one overlay playing (red flash, orange triple or orange steady). The
      // sequencer is read from the timer handler and written from the main
      // thread; see playLedPattern(). While it plays, m_led_timer is the only
      // writer of all three LEDs; the main loop writes them once it has ended.
      LedSequencer m_led_sequencer;
      struct k_timer m_led_timer;

      // Display only: the device disarmed itself (armed trigger, fail-safe
      // disarm) and no authenticated command has been accepted since. Shows
      // steady orange. Never affects the arm state or the output.
      bool m_fault_display_latched;

      // False until initLeds() has configured the three LED pins. A warning raised
      // before then (the boot pin check) starts its sequencer at once but its
      // timer only after initLeds() - see Run().
      bool m_leds_initialised;

      bool m_initialised;
  };

}
