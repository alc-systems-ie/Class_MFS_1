#pragma once

#include <cstdint>

#include <zephyr/device.h>
#include <zephyr/drivers/gpio.h>

#include "adxl367.hpp"
#include "command_scanner.hpp"
#include "npm2100.hpp"

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

      // Writes the two LED pins. Takes the states directly so the caller can log
      // exactly what is applied, rather than each recomputing and disagreeing.
      int applyLeds(bool ledA, bool ledB);

      // Derives m_output_active. The single place the arm state and the
      // accelerometer are combined — see IsOutputActive().
      void updateOutputState();

      void setArmState(ArmState state);

      void toggleArmState();

      const struct device* m_i2c_bus;
      Npm2100 m_pmic;
      Adxl367 m_accelerometer;
      CommandScanner m_scanner;
      ArmState m_arm_state;

      // True when the device was armed while the ADXL367 was already awake. That
      // assertion belongs to motion from BEFORE arming, so it must not count as a
      // trigger; it is suppressed until INT1 de-asserts and a fresh edge arrives.
      bool m_ignore_stale_trigger;

      // The definitive output state. Written only by updateOutputState(), read
      // only via IsOutputActive().
      bool m_output_active;

      // Consecutive loop ticks with the ADXL awake, for the stuck-AWAKE watchdog.
      uint32_t m_awake_ticks;

      bool m_initialised;
  };

}
