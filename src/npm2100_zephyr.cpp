// Zephyr glue for the Npm2100 driver.

#include "npm2100_zephyr.hpp"

#include <zephyr/device.h>
#include <zephyr/drivers/i2c.h>
#include <zephyr/logging/log.h>

LOG_MODULE_REGISTER(npm2100, LOG_LEVEL_INF);

namespace alc::npm2100_zephyr
{

  static int ZephyrRead(uint8_t devAddr, uint8_t regAddr, uint8_t* data, size_t len, void* ctx)
  {
    const struct device* i2c = static_cast<const struct device*>(ctx);
    return i2c_burst_read(i2c, devAddr, regAddr, data, len);
  }

  static int ZephyrWrite(uint8_t devAddr, const uint8_t* data, size_t len, void* ctx)
  {
    const struct device* i2c = static_cast<const struct device*>(ctx);
    return i2c_write(i2c, data, len, devAddr);
  }

  static void ZephyrLog(int level, const char* fmt, ...)
  {
    va_list ap;
    va_start(ap, fmt);
    switch (level) {
      case 3:
        log_generic(LOG_LEVEL_ERR, fmt, ap);
        break;
      case 2:
        log_generic(LOG_LEVEL_WRN, fmt, ap);
        break;
      case 1:
        log_generic(LOG_LEVEL_INF, fmt, ap);
        break;
      default:
        log_generic(LOG_LEVEL_DBG, fmt, ap);
        break;
    }
    va_end(ap);
  }

  alc::Npm2100::I2cTransport MakeZephyrTransport(const struct device* i2cBus)
  {
    return alc::Npm2100::I2cTransport {
      .Read  = &ZephyrRead,
      .Write = &ZephyrWrite,
      .Log   = &ZephyrLog,
      .ctx   = const_cast<struct device*>(i2cBus),
    };
  }

} // namespace alc::npm2100_zephyr
