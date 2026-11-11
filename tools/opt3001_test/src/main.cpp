#include <zephyr/device.h>
#include <zephyr/drivers/i2c.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

#include "npm2100.hpp"
#include "npm2100_zephyr.hpp"

LOG_MODULE_REGISTER(opt3001_test, LOG_LEVEL_INF);

namespace alc
{

  namespace
  {
    const struct device* const s_pmic_bus { DEVICE_DT_GET(DT_NODELABEL(i2c21)) };
    const struct device* const s_sensor_bus { DEVICE_DT_GET(DT_NODELABEL(i2c22)) };

    // OPT3001 (TI SBOS681), ADDR pin to GND.
    constexpr uint16_t M_OPT3001_ADDRESS { 0x44 };
    constexpr uint8_t M_REG_RESULT { 0x00 };
    constexpr uint8_t M_REG_CONFIG { 0x01 };
    constexpr uint8_t M_REG_MANUFACTURER_ID { 0x7E };
    constexpr uint8_t M_REG_DEVICE_ID { 0x7F };
    constexpr uint16_t M_EXPECTED_MANUFACTURER_ID { 0x5449 }; // "TI".
    constexpr uint16_t M_EXPECTED_DEVICE_ID { 0x3001 };

    // RN = 1100 (automatic full-scale), CT = 1 (800 ms), M = 11 (continuous), L = 1 (latched).
    constexpr uint16_t M_CONFIG_CONTINUOUS_AUTO { 0xCE10 };
    constexpr uint16_t M_CONFIG_READY_MASK { 0x0080 };    // CRF.
    constexpr uint16_t M_CONFIG_OVERFLOW_MASK { 0x0100 }; // OVF.

    constexpr uint8_t M_RESULT_EXPONENT_SHIFT { 12 };
    constexpr uint16_t M_RESULT_MANTISSA_MASK { 0x0FFF };
    constexpr uint32_t M_CENTILUX_PER_LUX { 100 };

    // I2C addresses scanned on the sensor bus: the 7-bit range outside the reserved blocks.
    constexpr uint16_t M_SCAN_FIRST_ADDRESS { 0x08 };
    constexpr uint16_t M_SCAN_LAST_ADDRESS { 0x77 };

    constexpr uint16_t M_LSOUT_MILLIVOLTS { 1800 };
    constexpr int32_t M_LSOUT_DISCHARGE_MS { 50 };
    constexpr int32_t M_SENSOR_POWER_ON_MS { 20 };
    constexpr int32_t M_READ_INTERVAL_MS { 1000 };

    int readRegister(uint8_t reg, uint16_t& value)
    {
      uint8_t buffer[2] { 0, 0 };
      int result { i2c_write_read(s_sensor_bus, M_OPT3001_ADDRESS, &reg, sizeof(reg), buffer, sizeof(buffer)) };

      if (result < 0) { return result; }
      value = static_cast<uint16_t>((buffer[0] << 8) | buffer[1]);
      return 0;
    }

    int writeRegister(uint8_t reg, uint16_t value)
    {
      const uint8_t buffer[3] { reg, static_cast<uint8_t>(value >> 8), static_cast<uint8_t>(value & 0xFF) };

      return i2c_write(s_sensor_bus, buffer, sizeof(buffer), M_OPT3001_ADDRESS);
    }

    // Stops the boot monitor and power-cycles LSOUT to 1.8 V, as App::initPmic() does.
    int initPmic(Npm2100& pmic)
    {
      int result { pmic.Init() };

      if (result < 0) {
        LOG_ERR("nPM2100 init failed: %d!", result);
        return result;
      }

      result = pmic.TimerStop();
      if (result < 0) {
        LOG_ERR("Failed to stop the nPM2100 boot monitor: %d!", result);
        return result;
      }
      LOG_INF("nPM2100 boot monitor stopped.");

      result = pmic.LdoSwDisable();
      if (result < 0) {
        LOG_ERR("Failed to disable LSOUT: %d!", result);
        return result;
      }
      k_msleep(M_LSOUT_DISCHARGE_MS);

      result = pmic.LdoSwSetOutputMode(Npm2100::LdoSwOutputMode::Ldo);
      if (result == 0) { result = pmic.LdoSwSetVoltage(M_LSOUT_MILLIVOLTS); }
      if (result == 0) { result = pmic.LdoSwSetPowerMode(Npm2100::LdoSwPowerMode::Hp); }
      if (result == 0) { result = pmic.LdoSwEnable(); }
      if (result < 0) {
        LOG_ERR("Failed to bring up LSOUT: %d!", result);
        return result;
      }
      k_msleep(M_SENSOR_POWER_ON_MS);

      LOG_INF("LSOUT up at %u mV.", M_LSOUT_MILLIVOLTS);
      return 0;
    }

    void scanSensorBus()
    {
      uint8_t probe { 0 };
      uint8_t found { 0 };

      for (uint16_t address = M_SCAN_FIRST_ADDRESS; address <= M_SCAN_LAST_ADDRESS; address++) {
        if (i2c_read(s_sensor_bus, &probe, sizeof(probe), address) == 0) {
          LOG_INF("i2c22: device answered at 0x%02x.", address);
          found++;
        }
      }
      LOG_INF("i2c22 scan complete: %u device(s).", found);
    }

    int run()
    {
      uint16_t manufacturerId { 0 };
      uint16_t deviceId { 0 };
      uint16_t config { 0 };
      uint16_t raw { 0 };
      uint32_t centilux { 0 };
      int result { 0 };

      if (!device_is_ready(s_pmic_bus) || !device_is_ready(s_sensor_bus)) {
        LOG_ERR("I2C bus not ready!");
        return -ENODEV;
      }

      LOG_INF("OPT3001 test boot.");

      Npm2100 pmic { npm2100_zephyr::MakeZephyrTransport(s_pmic_bus) };
      // Carry on regardless: the OPT3001 may not be on LSOUT, and the log shows what failed.
      initPmic(pmic);

      scanSensorBus();

      result = readRegister(M_REG_MANUFACTURER_ID, manufacturerId);
      if (result == 0) { result = readRegister(M_REG_DEVICE_ID, deviceId); }
      if (result < 0) {
        LOG_ERR("OPT3001 not answering at 0x%02x: %d!", M_OPT3001_ADDRESS, result);
        return result;
      }
      LOG_INF("OPT3001 manufacturer ID 0x%04x (%s), device ID 0x%04x (%s).", manufacturerId,
              manufacturerId == M_EXPECTED_MANUFACTURER_ID ? "OK" : "UNEXPECTED", deviceId, deviceId == M_EXPECTED_DEVICE_ID ? "OK" : "UNEXPECTED");

      result = writeRegister(M_REG_CONFIG, M_CONFIG_CONTINUOUS_AUTO);
      if (result < 0) {
        LOG_ERR("OPT3001 config write failed: %d!", result);
        return result;
      }
      LOG_INF("OPT3001 configured: continuous, auto range, 800 ms conversions.");

      while (true) {
        k_msleep(M_READ_INTERVAL_MS);

        result = readRegister(M_REG_CONFIG, config);
        if (result == 0) { result = readRegister(M_REG_RESULT, raw); }
        if (result < 0) {
          LOG_ERR("OPT3001 read failed: %d!", result);
          continue;
        }

        // lux = 0.01 x 2^E x R, so centilux = R << E.
        centilux = static_cast<uint32_t>(raw & M_RESULT_MANTISSA_MASK) << (raw >> M_RESULT_EXPONENT_SHIFT);
        LOG_INF("Light %u.%02u lux (raw 0x%04x, config 0x%04x%s%s).", centilux / M_CENTILUX_PER_LUX, centilux % M_CENTILUX_PER_LUX, raw, config,
                (config & M_CONFIG_READY_MASK) ? ", ready" : "", (config & M_CONFIG_OVERFLOW_MASK) ? ", OVERFLOW" : "");
      }

      return 0;
    }
  }

}

int main()
{ return alc::run(); }
