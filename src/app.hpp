#pragma once

#include <cstdint>

#include <zephyr/device.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/kernel.h>

#include "access_control.hpp"
#include "adxl367.hpp"
#include "arm_policy.hpp"
#include "command_scanner.hpp"
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
   */
  class App
  {
    public:
      App();

      /** @brief Brings up the peripherals and enters the main loop. Does not return. */
      int Run();

      /**
       * @brief The device output state — the ONLY sanctioned trigger source.
       *
       * True only when the device is Active AND the ADXL367 is reporting motion
       * that began after arming. **m_arm_state is definitive**; the accelerometer
       * is only ever ANDed with it.
       *
       * Every consumer must use this. Nothing may read INT1, the AWAKE bit or the
       * ADXL367 directly and act on it: in the product this output switches a
       * voltage, so a device that fires while deactivated is dangerous. See
       * App::updateOutputState() and docs/v1-scope.md section 1.0.
       */
      bool IsOutputActive() const { return m_output_active; }

    private:
      /** @brief Whether the sensor is armed. Cold start defaults to Inactive. */
      enum class ArmState : uint8_t { Inactive = 0, Active = 1 };

      // Drives the four unused nRF21540 control pins low. The FEM is fitted on the
      // bespoke alc_drawer_master board but MFS_1 does not use it; a floating PDN
      // would leave it in an indeterminate state instead of power-down.
      int parkFrontEndModule();

      int initLeds();

      // Probes the PMIC, reports the reset reason, disables the SHPHLD power-off
      // path, and brings up LSOUT at 1.8 V in Ultra-Low Power mode for the ADXL367.
      int initPmic();

      // Drops LDOSW to Ultra-Low Power once the ADXL367 is configured. The rail is
      // brought up in High Power because the ADXL367 needs >250 uA during power-up
      // for correct fuse loading.
      int lowerLsoutToUlp();

      int initAccelerometer();

      // Configures the ADXL367 and proves it is reporting inactivity. Called
      // BEFORE m_arm_state goes Active, so the device cannot come up armed on an
      // assertion that predates arming.
      int enableAccelerometer();

      // Takes the output to 0 through updateOutputState(), then puts the ADXL367
      // in standby. Called AFTER m_arm_state goes Inactive, so the output is
      // already derived low before the part is stopped.
      int disableAccelerometer();

      // Writes LED A. Split from LED B so the main loop can skip this call while
      // the LED timer owns the pin - see ledSequencerActive in Run().
      int applyLedA(bool ledA);

      // Writes LED B. Always called from the main loop; nothing else writes it.
      int applyLedB(bool ledB);

      // Derives m_output_active. The single place the arm state and the
      // accelerometer are combined — see IsOutputActive().
      void updateOutputState();

      void setArmState(ArmState state);

      // Stands the ADXL367 down and starts the PMIC timer for the cooldown between
      // counted activations. No-op when the cooldown is zero.
      int beginCooldown();

      // Polls the PMIC timer; on expiry re-arms the ADXL367 through the full
      // bootstrap so the engine cannot inherit a level from the blanking window.
      void serviceCooldown();

      /**
       * @brief True only when BOTH witnesses agree no delay is running.
       *
       * A flag left set with a dead timer now waits for the deadline rather
       * than firing early - see m_delay_deadline_ms and the expiry commit in
       * updateOutputState(). A running timer with a cleared flag still blocks
       * firing. Both failure directions are safe, which is the whole reason
       * for using two witnesses of different kinds.
       */
      bool delayPermitsFiring() const;

      // Installed into OutputSwitch as the second, independent layer.
      static bool interlockThunk(void* context);

      void beginDelay();
      void cancelDelay();

      // Confirms the scanner is actually running, retries at
      // M_SCAN_SERVICE_INTERVAL_MS if not, and updates m_delay_scan_lost every
      // tick while a delay is pending - see the expiry commit in
      // updateOutputState(). Called every main-loop tick.
      void serviceScanHealth();

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

      ArmState m_arm_state;

      // True when the ADXL367 was still awake immediately after being configured
      // for arming. That assertion belongs to motion from BEFORE arming, so it
      // must not count as a trigger; it is suppressed until INT1 de-asserts and a
      // fresh edge arrives. Belt and braces - the configuration bootstrap drives
      // AWAKE low, so this should not normally be set.
      bool m_ignore_stale_trigger;

      // The definitive output state. Written only by updateOutputState(), read
      // only via IsOutputActive().
      bool m_output_active;

      // Consecutive loop ticks with the ADXL awake, for the stuck-AWAKE watchdog.
      uint32_t m_awake_ticks;

      // The engineer-settable parameters, NVS-backed.
      Settings m_settings;

      // Latched by the detection engine when the activation count reaches the
      // configured threshold; cleared when AWAKE de-asserts. NOT derived in
      // updateOutputState() - the engine zeroes the count when it latches, so
      // deriving this from the count would take the output false immediately.
      bool m_detection_met;

      // Activations seen since the last trigger or deactivation. Does not expire.
      uint8_t m_activation_count;

      // True while the ADXL is standing down for a cooldown window.
      bool m_in_cooldown;

      // Previous INT1 level, for edge detection. The engine counts RISING edges,
      // not levels - a level would count the same activation on every loop tick.
      bool m_previous_awake;

      // Uptime at which the cooldown is forced over regardless of what the PMIC
      // reports, so a TimerIsExpired() fault or a timer that never expires cannot
      // strand the device in standby forever. See serviceCooldown().
      int64_t m_cooldown_deadline_ms;

      // True once a failed re-arm after cooldown has been logged, so a retry that
      // keeps failing logs once rather than every 100 ms tick.
      bool m_cooldown_rearm_failed;

      // The cooldown window itself is over and only the re-arm remains. Once set,
      // serviceCooldown() stops consulting the PMIC - it already cleared the
      // timer's expiry event on the transition - and just retries the re-arm.
      bool m_cooldown_expired;

      // Earliest uptime at which the next re-arm attempt may run, so a failing
      // re-arm retries at M_COOLDOWN_RETRY_MS rather than every 100 ms tick.
      int64_t m_cooldown_next_retry_ms;

      // The delay's two independent witnesses. Deliberately different in kind so
      // that either being wrong still BLOCKS firing - see delayPermitsFiring().
      bool m_delay_pending;
      struct k_timer m_delay_timer;

      // Held for the whole delay so the SoC cannot enter a deeper state - only
      // takes effect when CONFIG_PM is enabled (it is off in this build, so
      // continuous scanning below is the mechanism that actually keeps a
      // disarm heard promptly). Battery life is explicitly not a factor while
      // a trigger is pending.
      bool m_delay_pm_lock_held;

      // Uptime at which a pending delay is considered genuinely expired.
      // k_timer_remaining_ticks() returns 0 both when a timer has expired and
      // when it was never armed, so this deadline is what tells the two apart
      // for the expiry commit in updateOutputState().
      int64_t m_delay_deadline_ms;

      // Set in beginDelay() if the scanner was not confirmed running at the
      // start of the delay, and every tick thereafter while the delay is
      // pending if it drops out. Does not suppress the trigger - Andy's
      // ruling is to prioritise the alarm - but is logged at the expiry
      // commit so a missed disarm is visible.
      bool m_delay_scan_lost;

      // Uptime of the last scanner health check - see serviceScanHealth().
      int64_t m_last_scan_service_ms;

      // True once a scan outage has been logged, so a scanner that stays down
      // logs once rather than every M_SCAN_SERVICE_INTERVAL_MS tick. Cleared
      // once the scanner is confirmed running again.
      bool m_scan_outage_logged;

      // Uptime until which a DELAYED trigger holds detection regardless of AWAKE.
      // Zero for an undelayed trigger, whose own AWAKE sets the duration.
      int64_t m_detection_hold_until_ms;

      // LED A acknowledgement patterns. The sequencer is read from the timer
      // handler and written from the main loop; see playLedPattern().
      LedSequencer m_led_sequencer;
      struct k_timer m_led_timer;

      // ONE-SHOT trigger. Set while the output is asserted; when it clears, the
      // trigger is complete and the main loop latches the device Inactive.
      bool m_trigger_fired;
      bool m_trigger_complete;

      bool m_initialised;
  };

}
