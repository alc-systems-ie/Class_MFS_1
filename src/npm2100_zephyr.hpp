#pragma once

#include "npm2100.hpp"

struct device;

namespace alc::npm2100_zephyr
{
  /** Build an I2cTransport bound to a Zephyr I²C bus device.
   *
   *  @param i2cBus Pointer to the Zephyr I²C controller device (e.g. from
   *                `DEVICE_DT_GET(DT_NODELABEL(i2c21))`).
   */
  alc::Npm2100::I2cTransport MakeZephyrTransport(const struct device* i2cBus);
}
