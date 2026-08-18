#pragma once

#include <cstdint>

#include <zephyr/device.h>

namespace alc
{

  /**
   * @brief Driver for the ADXL367 accelerometer over I2C (plain register pointer).
   *
   * Configured for **loop mode** referenced activity/inactivity: the part manages
   * the activity to inactivity cycle itself and acknowledges its own interrupts,
   * so the host needs no timer. The AWAKE status bit is mapped to INT1, which
   * therefore asserts on motion and de-asserts after the configured stillness
   * period. That signal is the LED B timeout — see docs/v1-scope.md section 1.
   *
   * @warning INT2 is hard-wired to the nPM2100 SHPHLD pin on this board and is
   *          deliberately configured to **never assert**. A sustained INT2
   *          assertion followed by release is the nPM2100 ship-mode gesture and
   *          would power the device off, and SHPHLD has a 1.9 V absolute maximum.
   *          See docs/v1-scope.md section 2.
   */
  class Adxl367
  {
    public:
      explicit Adxl367(const struct device* i2c)
          : m_i2c(i2c)
          , m_address(0)
      {}

      /**
       * @brief Probe the part on 0x1D then 0x53 and cache the answering address.
       * @return 0 on success; -ENODEV if DEVID/PARTID never match.
       */
      int Init();

      /**
       * @brief Soft-reset and configure loop-mode referenced activity/inactivity.
       *
       * @param threshold           Activity threshold, 13-bit, 0.25 mg/LSB at 2 g.
       * @param activitySamples     Sustained samples required to latch activity.
       * @param inactivityThreshold Inactivity threshold. MUST be wide enough to
       *                            cover any orientation change, or the part
       *                            sticks awake permanently — see the
       *                            implementation note.
       * @param inactivitySecs      Stillness before the part returns to activity
       *                            detection. This is the AWAKE de-assert delay.
       * @return 0 on success; negative errno on transport failure.
       */
      int ConfigureLoopMode(uint16_t threshold, uint8_t activitySamples, uint16_t inactivityThreshold, uint8_t inactivitySecs);

      /**
       * @brief Read the live AWAKE state from STATUS.
       *
       * @param awake Out: true if the part has seen activity and has not yet
       *              timed out into inactivity.
       * @return 0 on success; negative errno on transport failure.
       * @note In loop mode the part acknowledges its own interrupts, so this is a
       *       state query rather than a latch clear.
       */
      int ReadAwake(bool& awake);

      /** @brief Put the part in standby; interrupts stop. */
      int Standby();

      /**
       * @brief Dump configuration registers, STATUS and axis data to the log.
       *
       * Diagnostic only. Reads back what the part actually holds rather than what we
       * believe we wrote, so a failed or ignored write is distinguishable from an
       * engine that is configured correctly but behaving unexpectedly.
       */
      int LogDiagnostics();

    private:
      int readRegister(uint8_t regAddr, uint8_t& value);
      int writeRegister(uint8_t regAddr, uint8_t value);

      // Writes a 13-bit threshold across its H/L register pair.
      int writeThreshold(uint8_t regHigh, uint8_t regLow, uint16_t value);

      // Writes the 16-bit inactivity sample count to TIME_INACT_H/L.
      int writeInactivityTime(uint16_t samples);

      const struct device* const m_i2c;
      uint8_t m_address;
  };

}
