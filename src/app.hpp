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

      // Applies the LED pair to the current arm state and trigger state.
      int refreshLeds(bool triggered);

      void setArmState(ArmState state);

      void toggleArmState();

      const struct device* m_i2c_bus;
      Npm2100 m_pmic;
      Adxl367 m_accelerometer;
      CommandScanner m_scanner;
      ArmState m_arm_state;
      bool m_initialised;
  };

}
