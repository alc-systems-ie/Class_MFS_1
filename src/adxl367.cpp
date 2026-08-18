#include <zephyr/drivers/i2c.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

#include "adxl367.hpp"

LOG_MODULE_REGISTER(adxl367, LOG_LEVEL_INF);

namespace
{

  constexpr uint8_t M_ADDR_PRIMARY { 0x1D };
  constexpr uint8_t M_ADDR_ALT { 0x53 };

  constexpr uint8_t M_REG_DEVID { 0x00 };
  constexpr uint8_t M_REG_PARTID { 0x02 };
  constexpr uint8_t M_DEVID_VALUE { 0xAD };
  constexpr uint8_t M_PARTID_VALUE { 0xF7 };

  constexpr uint8_t M_REG_XDATA { 0x08 }; // XDATA/YDATA/ZDATA: top 8 bits of each axis.
  constexpr uint8_t M_REG_STATUS { 0x0B };
  constexpr uint8_t M_STATUS_AWAKE_MASK { 0x40 }; // Bit 6: 1 = active, 0 = inactive.

  constexpr uint8_t M_REG_SOFT_RESET { 0x1F };
  constexpr uint8_t M_RESET_CODE { 0x52 };

  constexpr uint8_t M_REG_THRESH_ACT_H { 0x20 };
  constexpr uint8_t M_REG_THRESH_ACT_L { 0x21 };
  constexpr uint8_t M_REG_TIME_ACT { 0x22 };
  constexpr uint8_t M_REG_THRESH_INACT_H { 0x23 };
  constexpr uint8_t M_REG_THRESH_INACT_L { 0x24 };
  constexpr uint8_t M_REG_TIME_INACT_H { 0x25 };
  constexpr uint8_t M_REG_TIME_INACT_L { 0x26 };
  constexpr uint8_t M_REG_ACT_INACT_CTL { 0x27 };

  // ACT_INACT_CTL (0x27) holds three multi-bit fields, NOT independent enable and
  // mode bits: [5:4] LINKLOOP, [3:2] INACT_EN, [1:0] ACT_EN. For both detection
  // fields 0b01 selects Absolute and 0b11 selects Referenced. Referenced is
  // mandatory here — in Absolute mode the ~1000 mg vertical gravity component
  // exceeds any practical threshold on the OR'd axes, so the part latches awake at
  // boot and never clears.
  constexpr uint8_t M_ACT_REFERENCED { 0x03 };   // [1:0] = 0b11.
  constexpr uint8_t M_INACT_REFERENCED { 0x0C }; // [3:2] = 0b11.
  constexpr uint8_t M_LINKLOOP_LOOP { 0x30 };    // [5:4] = 0b11.
  constexpr uint8_t M_ACT_INACT_DISABLED { 0x00 };
  constexpr uint8_t M_ACT_INACT_LOOP_REFERENCED { M_LINKLOOP_LOOP | M_INACT_REFERENCED | M_ACT_REFERENCED };

  constexpr uint8_t M_REG_INTMAP1_LOWER { 0x2A };
  constexpr uint8_t M_REG_INTMAP2_LOWER { 0x2B };
  constexpr uint8_t M_REG_INTMAP1_UPPER { 0x3A };
  constexpr uint8_t M_REG_INTMAP2_UPPER { 0x3B };

  // INTMAP bit positions mirror STATUS for bits 6..0, with bit 7 selecting the pin
  // polarity.
  constexpr uint8_t M_INT_AWAKE_MASK { 0x40 };
  constexpr uint8_t M_INT_ACTIVE_LOW { 0x80 };
  constexpr uint8_t M_INT_NONE { 0x00 };

  // FILTER_CTL reset value: [7:6] RANGE 00 = +/-2 g, [5] I2C_HS set as it powers
  // up, [2:0] ODR 011 = 100 Hz. Writing the reset value keeps I2C_HS rather than
  // clearing it as a bare ODR write would.
  constexpr uint8_t M_REG_FILTER_CTL { 0x2C };
  constexpr uint8_t M_FILTER_2G_100HZ { 0x23 };
  constexpr uint16_t M_ODR_HZ { 100 };

  constexpr uint8_t M_REG_POWER_CTL { 0x2D };
  constexpr uint8_t M_POWER_STANDBY { 0x00 };
  // Literal value from step 7 of the datasheet's loop mode initialization
  // routine: autosleep (bit 2) plus measurement mode. Note the register table
  // documents MEASURE [1:0] as 0b10, so 0x07 sets a [1:0] the table does not
  // list; the routine is followed verbatim rather than second-guessed, and the
  // diagnostic dump reports what the part actually latched.
  constexpr uint8_t M_POWER_MEASURE_AUTOSLEEP { 0x07 };

  // Loop mode bootstrap values, datasheet steps 1-4. Activity threshold below the
  // noise floor (1 LSB) and inactivity threshold at full scale (13-bit max, well
  // over 1 g), both with zero timers, so the engine cycles immediately and
  // captures a valid reference.
  // THRESH_INACT MUST cover the largest possible orientation change, which is 2 g
  // (an axis swinging from +1 g to -1 g). Setting it near the activity threshold
  // permanently sticks the part awake, because the inactivity reference is captured
  // when ACTIVITY fires and, per the datasheet, "in linked and looped mode ... the
  // inactivity threshold cannot be continuously updated ... the accelerometer does
  // not detect inactivity until the acceleration input returns to within the
  // inactivity threshold". Put the board down in a different orientation and it
  // never returns, so AWAKE never clears.
  //
  // Diagnosed on hardware 2026-08-18 with THRESH_INACT = THRESH_ACT = 300 (75 mg):
  // AWAKE held for 50 s and then indefinitely after the board was re-oriented.
  //
  // At full scale inactivity becomes a genuine "5 s since the last activity" timer,
  // which is exactly the specified behaviour and is immune to orientation.
  constexpr uint16_t M_THRESH_INACT_MIN_SAFE { 8191 }; // 13-bit full scale, ~2 g.

  constexpr uint16_t M_THRESH_ACT_BOOTSTRAP { 1 };
  constexpr uint16_t M_THRESH_INACT_BOOTSTRAP { 0x1FFF };
  constexpr uint8_t M_TIME_BOOTSTRAP { 0 };

  constexpr uint32_t M_RESET_SETTLE_MS { 8 };
  constexpr uint32_t M_MEASURE_SETTLE_MS { 100 };
  constexpr uint8_t M_PROBE_ATTEMPTS { 5 };
  constexpr uint32_t M_PROBE_RETRY_MS { 10 };

  // Threshold registers hold an 11-bit value split H[10:6] / L[5:0] << 2.
  constexpr uint8_t M_THRESH_H_SHIFT { 6 };
  constexpr uint8_t M_THRESH_L_SHIFT { 2 };
  constexpr uint16_t M_THRESH_L_MASK { 0x3F };
  constexpr uint8_t M_THRESH_H_MASK { 0x7F };

  // The part boots AWAKE, so arming ends with a wait for it to fall asleep. The
  // budget must exceed the configured inactivity period plus the measurement settle.
  //
  // NOTE: every register in 0x00-0x2D must be written in STANDBY. The datasheet is
  // explicit that "changes made during measurement mode may only be effective for
  // part of a measurement", so nothing here may be deferred until after POWER_CTL
  // enables measurement. An earlier revision wrote ACT_INACT_CTL and TIME_INACT
  // while running and neither took reliable effect.
  constexpr uint32_t M_SETTLE_POLL_MS { 50 };
  constexpr uint32_t M_SETTLE_MARGIN_MS { 3000 };

  constexpr uint8_t M_STATUS_ERR_USER_REGS { 0x80 };

}

namespace alc
{

  int Adxl367::readRegister(uint8_t regAddr, uint8_t& value)
  {
    return i2c_reg_read_byte(m_i2c, m_address, regAddr, &value);
  }

  int Adxl367::writeRegister(uint8_t regAddr, uint8_t value)
  {
    return i2c_reg_write_byte(m_i2c, m_address, regAddr, value);
  }

  int Adxl367::Init()
  {
    constexpr uint8_t candidates[] { M_ADDR_PRIMARY, M_ADDR_ALT };

    if (!device_is_ready(m_i2c)) {
      LOG_ERR("I2C bus not ready for ADXL367!");
      return -ENODEV;
    }

    // Retried because the part answers only once its supply has come up and its
    // fuses have loaded. The caller allows for that before calling, but a silent
    // one-shot failure here is indistinguishable from a dead part, so report how
    // many attempts it took.
    for (uint8_t attempt { 0 }; attempt < M_PROBE_ATTEMPTS; ++attempt) {
      for (uint8_t candidate : candidates) {
        uint8_t deviceId { 0 };
        uint8_t partId { 0 };

        if (i2c_reg_read_byte(m_i2c, candidate, M_REG_DEVID, &deviceId) < 0) { continue; }
        if (i2c_reg_read_byte(m_i2c, candidate, M_REG_PARTID, &partId) < 0) { continue; }

        if (deviceId == M_DEVID_VALUE && partId == M_PARTID_VALUE) {
          m_address = candidate;
          if (attempt > 0) { LOG_WRN("ADXL367 answered only on probe attempt %u - supply settling is marginal!", attempt + 1U); }
          LOG_INF("ADXL367 confirmed at 0x%02X.", candidate);
          return 0;
        }
        LOG_DBG("ADXL367 probe 0x%02X: DEVID 0x%02X PARTID 0x%02X.", candidate, deviceId, partId);
      }
      k_msleep(M_PROBE_RETRY_MS);
    }

    LOG_ERR("ADXL367 not found on i2c21 after %u attempts!", M_PROBE_ATTEMPTS);
    return -ENODEV;
  }

  int Adxl367::writeThreshold(uint8_t regHigh, uint8_t regLow, uint16_t value)
  {
    // 13-bit unsigned, split H[12:6] / L[5:0] << 2.
    int result { writeRegister(regHigh, static_cast<uint8_t>((value >> M_THRESH_H_SHIFT) & M_THRESH_H_MASK)) };

    if (result < 0) { return result; }
    return writeRegister(regLow, static_cast<uint8_t>((value & M_THRESH_L_MASK) << M_THRESH_L_SHIFT));
  }

  int Adxl367::writeInactivityTime(uint16_t samples)
  {
    int result { writeRegister(M_REG_TIME_INACT_H, static_cast<uint8_t>(samples >> 8)) };

    if (result < 0) { return result; }
    return writeRegister(M_REG_TIME_INACT_L, static_cast<uint8_t>(samples & 0xFF));
  }

  int Adxl367::ConfigureLoopMode(uint16_t threshold, uint8_t activitySamples, uint16_t inactivityThreshold, uint8_t inactivitySecs)
  {
    int result { 0 };
    uint8_t threshHigh { 0 };
    uint8_t threshLow { 0 };
    uint8_t status { 0 };
    uint16_t inactivitySamples { 0 };
    uint32_t settleBudgetMs { (inactivitySecs * MSEC_PER_SEC) + M_SETTLE_MARGIN_MS };
    bool settled { false };

    if (m_address == 0) {
      LOG_ERR("ADXL367 configure before successful Init!");
      return -ENODEV;
    }

    // Standby, then soft-reset and wait for the part to come back.
    result = writeRegister(M_REG_POWER_CTL, M_POWER_STANDBY);
    if (result == 0) { result = writeRegister(M_REG_SOFT_RESET, M_RESET_CODE); }
    if (result < 0) {
      LOG_ERR("ADXL367 reset failed: %d!", result);
      return result;
    }
    k_msleep(M_RESET_SETTLE_MS);

    // Steps 1-4 of the datasheet's loop mode initialization routine. The engine
    // must be made to cycle once before it holds a valid reference, and this is
    // how ADI documents doing it: an activity threshold BELOW the noise floor and
    // an inactivity threshold ABOVE 1 g, both with zero timers, so activity and
    // then inactivity both trigger immediately. That first cycle is what captures
    // the reference and drives AWAKE low. The real thresholds go in at step 9,
    // once the engine is running.
    result = writeThreshold(M_REG_THRESH_ACT_H, M_REG_THRESH_ACT_L, M_THRESH_ACT_BOOTSTRAP);
    if (result == 0) { result = writeRegister(M_REG_TIME_ACT, M_TIME_BOOTSTRAP); }
    if (result == 0) { result = writeThreshold(M_REG_THRESH_INACT_H, M_REG_THRESH_INACT_L, M_THRESH_INACT_BOOTSTRAP); }
    if (result == 0) { result = writeInactivityTime(M_TIME_BOOTSTRAP); }

    // Step 5: activity and inactivity referenced, loop mode.
    if (result == 0) { result = writeRegister(M_REG_ACT_INACT_CTL, M_ACT_INACT_LOOP_REFERENCED); }

    // Step 6, "other customer settings". FILTER_CTL is written with its reset
    // value: +/-2 g, 100 Hz ODR, and I2C_HS left set as it powers up.
    if (result == 0) { result = writeRegister(M_REG_FILTER_CTL, M_FILTER_2G_100HZ); }

    // INT1 tracks AWAKE, active-low to match adxl-int1-gpios in the board overlay.
    if (result == 0) { result = writeRegister(M_REG_INTMAP1_LOWER, M_INT_ACTIVE_LOW | M_INT_AWAKE_MASK); }
    if (result == 0) { result = writeRegister(M_REG_INTMAP1_UPPER, M_INT_NONE); }

    // INT2 goes to the PMIC SHPHLD pin. Map nothing to it, and set the active-low
    // polarity bit so the pin idles HIGH rather than LOW — an idle-low INT2 would
    // hold SHPHLD asserted, which is the ship-mode press. See the class comment.
    if (result == 0) { result = writeRegister(M_REG_INTMAP2_LOWER, M_INT_ACTIVE_LOW | M_INT_NONE); }
    if (result == 0) { result = writeRegister(M_REG_INTMAP2_UPPER, M_INT_NONE); }
    if (result < 0) {
      LOG_ERR("ADXL367 config write failed: %d!", result);
      return result;
    }

    // Step 7: measurement mode plus autosleep, using the literal value from the
    // datasheet routine. AUTOSLEEP is what gives AWAKE meaning — it moves the part
    // between measurement and wake-up mode as the loop detects activity and
    // inactivity, and AWAKE reports which state it is in. It also drops the part to
    // wake-up mode (~180 nA) while inactive rather than holding measurement mode
    // (~0.89 uA), improving on the figure in docs/power-budget.md section 3.
    result = writeRegister(M_REG_POWER_CTL, M_POWER_MEASURE_AUTOSLEEP);
    if (result < 0) {
      LOG_ERR("ADXL367 measure-mode enable failed: %d!", result);
      return result;
    }
    k_msleep(M_MEASURE_SETTLE_MS);

    // Wait for the engine to resolve out of its boot-awake state into inactivity.
    // The budget must exceed the configured inactivity period, since that is how
    // long stillness must persist before the part sleeps.
    settled = false;
    for (uint32_t elapsed { 0 }; elapsed < settleBudgetMs; elapsed += M_SETTLE_POLL_MS) {
      bool awake { true };

      if (ReadAwake(awake) == 0 && !awake) {
        settled = true;
        break;
      }
      k_msleep(M_SETTLE_POLL_MS);
    }

    if (settled) {
      LOG_INF("ADXL367 loop bootstrapped, AWAKE cleared - reference is valid.");
    } else {
      LOG_WRN("ADXL367 AWAKE did not clear in %u ms - loop bootstrap failed!", settleBudgetMs);
    }

    // Step 9: install the real thresholds and timers now the engine is cycling.
    // The datasheet routine does this in measurement mode, which is why the
    // general "configure in standby" guidance does not apply to 0x20-0x26 here.
    threshHigh        = static_cast<uint8_t>((threshold >> M_THRESH_H_SHIFT) & M_THRESH_H_MASK);
    threshLow         = static_cast<uint8_t>((threshold & M_THRESH_L_MASK) << M_THRESH_L_SHIFT);
    inactivitySamples = static_cast<uint16_t>(inactivitySecs * M_ODR_HZ);

    // THRESH_INACT is deliberately NOT the activity threshold. See the note above
    // M_THRESH_INACT_MIN_SAFE.
    result = writeThreshold(M_REG_THRESH_ACT_H, M_REG_THRESH_ACT_L, threshold);
    if (result == 0) { result = writeRegister(M_REG_TIME_ACT, activitySamples); }
    if (result == 0) { result = writeThreshold(M_REG_THRESH_INACT_H, M_REG_THRESH_INACT_L, inactivityThreshold); }
    if (result == 0) { result = writeInactivityTime(inactivitySamples); }
    if (result < 0) {
      LOG_ERR("ADXL367 threshold install failed: %d!", result);
      return result;
    }

    result = readRegister(M_REG_STATUS, status);
    if (result == 0 && (status & M_STATUS_ERR_USER_REGS) != 0) {
      LOG_ERR("ADXL367 reports ERR_USER_REGS - configuration rejected!");
      return -EIO;
    }

    LOG_INF("ADXL367 loop mode armed: thresh %u, act %u samples, inact %u s (%u samples).", threshold, activitySamples, inactivitySecs,
            inactivitySamples);
    LOG_INF("INT1 tracks AWAKE (active-low). INT2 unmapped, idling high - SHPHLD safe.");
    return 0;
  }

  int Adxl367::ReadAwake(bool& awake)
  {
    uint8_t status { 0 };
    int result { readRegister(M_REG_STATUS, status) };

    if (result < 0) {
      LOG_ERR("ADXL367 STATUS read failed: %d!", result);
      return result;
    }

    awake = (status & M_STATUS_AWAKE_MASK) != 0;
    return 0;
  }

  int Adxl367::Standby()
  {
    // Park BOTH interrupt pins before stopping the engine, and do it here rather
    // than relying on the reset defaults.
    //
    // INT2 is wired to the nPM2100 SHPHLD pin. What makes that pin safe is the
    // active-low POLARITY bit, which forces it to idle HIGH; the INTMAP2 reset
    // value of 0x00 does not set it. ConfigureLoopMode() writes it, but a device
    // that boots and is never activated would then sit at the reset default
    // indefinitely, since the part is now held in standby until it is armed. A
    // hazard the datasheet calls out must not depend on how soon someone happens
    // to arm the device - see docs/v1-scope.md section 2.
    //
    // Observed 2026-08-18: an unmapped INT1 read physically HIGH at 0x00, so the
    // reset default may well be harmless in practice. That is an inference from
    // the sibling pin, not a guarantee, and this costs four register writes.
    int result { writeRegister(M_REG_INTMAP2_LOWER, M_INT_ACTIVE_LOW | M_INT_NONE) };

    if (result == 0) { result = writeRegister(M_REG_INTMAP2_UPPER, M_INT_NONE); }

    // INT1 gets the same treatment. Active-low with nothing mapped idles HIGH,
    // which the GPIO_ACTIVE_LOW spec in the overlay reads back as de-asserted.
    if (result == 0) { result = writeRegister(M_REG_INTMAP1_LOWER, M_INT_ACTIVE_LOW | M_INT_NONE); }
    if (result == 0) { result = writeRegister(M_REG_INTMAP1_UPPER, M_INT_NONE); }
    if (result < 0) {
      LOG_ERR("ADXL367 interrupt park failed: %d!", result);
      return result;
    }

    return writeRegister(M_REG_POWER_CTL, M_POWER_STANDBY);
  }

  int Adxl367::LogDiagnostics()
  {
    uint8_t status { 0 };
    int8_t axes[3] {};
    uint8_t config[8] {};
    uint8_t maps[2] {};
    uint8_t filterPower[2] {};
    int result { 0 };

    if (m_address == 0) { return -ENODEV; }

    // XDATA/YDATA/ZDATA at 0x08-0x0A are the top 8 bits of each 14-bit axis, which
    // is ample for judging whether the part is sampling and roughly where 1 g sits.
    result = i2c_burst_read(m_i2c, m_address, M_REG_XDATA, reinterpret_cast<uint8_t*>(axes), sizeof(axes));
    if (result == 0) { result = readRegister(M_REG_STATUS, status); }
    // THRESH_ACT_H .. ACT_INACT_CTL, i.e. 0x20-0x27.
    if (result == 0) { result = i2c_burst_read(m_i2c, m_address, M_REG_THRESH_ACT_H, config, sizeof(config)); }
    if (result == 0) { result = i2c_burst_read(m_i2c, m_address, M_REG_INTMAP1_LOWER, maps, sizeof(maps)); }
    if (result == 0) { result = i2c_burst_read(m_i2c, m_address, M_REG_FILTER_CTL, filterPower, sizeof(filterPower)); }
    if (result < 0) {
      LOG_ERR("ADXL367 diagnostic read failed: %d!", result);
      return result;
    }

    LOG_INF("ADXL367 STATUS 0x%02X (AWAKE %u, INACT %u, ACT %u, ERR %u)", status, (status & M_STATUS_AWAKE_MASK) ? 1U : 0U, (status & 0x20) ? 1U : 0U,
            (status & 0x10) ? 1U : 0U, (status & M_STATUS_ERR_USER_REGS) ? 1U : 0U);
    LOG_INF("ADXL367 axes X %d Y %d Z %d (8-bit)", axes[0], axes[1], axes[2]);
    LOG_INF("ADXL367 0x20-0x27: %02X %02X %02X %02X %02X %02X %02X %02X", config[0], config[1], config[2], config[3], config[4], config[5], config[6],
            config[7]);
    LOG_INF("ADXL367 INTMAP1 0x%02X INTMAP2 0x%02X FILTER_CTL 0x%02X POWER_CTL 0x%02X", maps[0], maps[1], filterPower[0], filterPower[1]);
    return 0;
  }

}
