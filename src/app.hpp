#pragma once

#include <cstdint>

#include <zephyr/device.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/kernel.h>

#include "access_control.hpp"
#include "adxl367.hpp"
#include "arm_policy.hpp"
#include "arming_sequence.hpp"
#include "command_scanner.hpp"
#include "detection_engine.hpp"
#include "device_clock.hpp"
#include "led_sequencer.hpp"
#include "npm2100.hpp"
#include "output_switch.hpp"
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

      // Drives the four unused nRF21540 control pins low. The FEM is fitted on the
      // bespoke alc_drawer_master board but MFS_1 does not use it; a floating PDN
      // would leave it in an indeterminate state instead of power-down.
      int parkFrontEndModule();

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

      // Writes LED A. Split from LED B so the main loop can skip this call while
      // the LED timer owns the pin - see ledSequencerActive in Run().
      int applyLedA(bool ledA);

      // Writes LED B. Split from LED A for the same reason: the main loop skips
      // this call while the warning timer owns the pin - see warningActive in Run().
      int applyLedB(bool ledB);

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
      // raised by the sequence through SignalWarning().
      void logArmingFailure();

      // The warning (arming sequence amendment section 4). Logs, and - until the
      // dedicated warning light is chosen - plays three long pulses on LED B in
      // every build, replacing any warning already playing. The one place a
      // dedicated pin is wired later. Must not call back into m_arming.
      void signalWarning(const char* reason, int result);

      // ArmingActions - see arming_sequence.hpp for each contract.
      //
      // NONE of these may call back into m_arming, directly or indirectly.
      // Commands are handled only from the main loop, in serviceCandidates(), and
      // nothing these call services the scanner queue.
      int DisableFirePins() override;
      int RestartDetection(bool armed) override;
      int EnableFirePins() override;
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
      // M_SCAN_SERVICE_INTERVAL_MS if not. Scanner loss during an armed delay is
      // tracked by the detection engine itself. Called every main-loop tick.
      void serviceScanHealth();

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

      void handleTimeSyncCandidate(const CommandScanner::Candidate& candidate, int64_t uptimeSecs);

      void handleCommandCandidate(const CommandScanner::Candidate& candidate, int64_t uptimeSecs);

      // Carries out an ACCEPTED command: arm transitions, settings, clock trim and
      // the LED A acknowledgement. Called only after AccessControl has persisted
      // the sequence number.
      void applyCommand(const AccessControl::Evaluation& evaluation, int64_t uptimeSecs);

      // Starts an LED A pattern and the 10 ms timer that renders it.
      void playLedPattern(LedPattern pattern);

      // k_timer expiry: renders LED A while a pattern plays, then stops itself.
      static void ledTimerHandler(struct k_timer* timer);

      // k_timer expiry: renders LED B while the warning plays, then stops itself.
      static void warningTimerHandler(struct k_timer* timer);

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

      // Uptime of the last scanner health check - see serviceScanHealth().
      int64_t m_last_scan_service_ms;

      // True once a scan outage has been logged, so a scanner that stays down
      // logs once rather than every M_SCAN_SERVICE_INTERVAL_MS tick. Cleared
      // once the scanner is confirmed running again.
      bool m_scan_outage_logged;

      // Logging only - the engine owns the cooldown. True between a
      // CooldownStarted event and the cooldown's end, so that a configure retry
      // (after a refused arm, a failed restart or a failed watchdog re-arm),
      // which the engine also runs as a "cooldown", is not logged as one.
      bool m_logging_cooldown;

      // Activation counting, cooldown, the delay, the detection period and the
      // stuck-AWAKE watchdog - in both arm states. Declared after everything its
      // DetectionHardware calls touch.
      DetectionEngine m_engine;

      // Inactive -> Arming -> Active, and every way back. The arm state lives
      // here and nowhere else. Declared after everything its ArmingActions calls
      // touch - the fire output, the engine and the output state.
      ArmingSequence m_arming;

      // LED A acknowledgement patterns. The sequencer is read from the timer
      // handler and written from the main loop; see playLedPattern().
      LedSequencer m_led_sequencer;
      struct k_timer m_led_timer;

      // The interim warning on LED B. Same ownership rule as LED A: while it
      // plays, m_warning_timer is LED B's only writer; the main loop writes LED B
      // only once it has ended. Started by signalWarning() from the main thread.
      LedSequencer m_warning_sequencer;
      struct k_timer m_warning_timer;

      // False until initLeds() has configured both LED pins. A warning raised
      // before then (the boot pin check) starts its sequencer at once but its
      // timer only after initLeds() - see Run().
      bool m_leds_initialised;

      bool m_initialised;
  };

}
