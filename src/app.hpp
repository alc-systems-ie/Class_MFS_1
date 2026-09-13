#pragma once

#include <cstdint>

#include <zephyr/device.h>
#include <zephyr/drivers/gpio.h>

#include "access_control.hpp"
#include "adxl367.hpp"
#include "command_scanner.hpp"
#include "device_clock.hpp"
#include "npm2100.hpp"
#include "output_switch.hpp"

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

      // Writes the two LED pins. Takes the states directly so the caller can log
      // exactly what is applied, rather than each recomputing and disagreeing.
      int applyLeds(bool ledA, bool ledB);

      // Derives m_output_active. The single place the arm state and the
      // accelerometer are combined — see IsOutputActive().
      void updateOutputState();

      void setArmState(ArmState state);

      // Parses the bench credentials from Kconfig, initialises PSA, runs the crypto
      // self-test and restores the access state. A failure leaves commands
      // disabled for the whole boot - see m_access_ready.
      int initAccess();

      // Drains the scanner queue. While the clock is invalid a candidate is offered
      // ONLY to the time-sync check; once valid, ONLY to AccessControl.
      void serviceCandidates();

      void handleTimeSyncCandidate(const CommandScanner::Candidate& candidate, int64_t uptimeSecs);

      void handleCommandCandidate(const CommandScanner::Candidate& candidate, int64_t uptimeSecs);

      const struct device* m_i2c_bus;
      Npm2100 m_pmic;
      Adxl367 m_accelerometer;
      CommandScanner m_scanner;

      // UTC for the access scheme. Invalid on every boot until a provisioner sync.
      DeviceClock m_clock;

      // The only judge of whether a candidate is an authentic, fresh command.
      AccessControl m_access;

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

      bool m_initialised;
  };

}
