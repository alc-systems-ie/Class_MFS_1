#pragma once

#include <zephyr/drivers/gpio.h>

namespace alc
{

/**
 * @brief Owns MFS_1 bring-up and the main loop.
 *
 * One Run() per boot. The SoC stays in System ON idle between scan windows —
 * the nPM2100 is never hibernated, so this does not return.
 */
class App
{
public:
  App();

  /** @brief Brings up the peripherals and enters the scan loop. Does not return. */
  int Run();

private:
  // Drives the four unused nRF21540 control pins low. The FEM is fitted on the
  // bespoke alc_drawer_master board but MFS_1 does not use it; a floating PDN
  // would leave it in an indeterminate state instead of power-down.
  int parkFrontEndModule();

  // Configures the LED GPIOs as outputs, both extinguished.
  int initLeds();

  bool m_initialised;
};

}
