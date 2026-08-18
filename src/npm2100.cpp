#include "npm2100.hpp"
#include "npm2100_regs.hpp"

#include <cerrno>
#include <cstdarg>
#include <cstring>

#define M_LOG_ERR(fmt, ...)                                                                                                                          \
  do {                                                                                                                                               \
    if (m_transport.Log) m_transport.Log(3, "Npm2100: " fmt __VA_OPT__(, ) __VA_ARGS__);                                                             \
  } while (0)
#define M_LOG_WRN(fmt, ...)                                                                                                                          \
  do {                                                                                                                                               \
    if (m_transport.Log) m_transport.Log(2, "Npm2100: " fmt __VA_OPT__(, ) __VA_ARGS__);                                                             \
  } while (0)

namespace alc
{
  using namespace npm2100::regs;

  Npm2100::Npm2100(const I2cTransport& transport)
      : m_transport(transport)
  {}

  int Npm2100::Init(bool clearResetReason)
  {
    // Step 1: probe — read SYSGDENSTATUS just to verify the chip ACKs.
    uint8_t probe { 0 };
    int rc = m_transport.Read(M_I2C_ADDRESS, M_RESET_SYSGDENSTATUS, &probe, 1, m_transport.ctx);
    if (rc < 0) {
      M_LOG_ERR("Init probe failed: %d", rc);
      return -EIO;
    }

    // Step 2: read & cache reset reason
    uint8_t resetReg { 0 };
    rc = m_transport.Read(M_I2C_ADDRESS, M_RESET_RESET, &resetReg, 1, m_transport.ctx);
    if (rc < 0) {
      M_LOG_ERR("Init reset-reason read failed: %d", rc);
      return -EIO;
    }
    m_brown_out    = (resetReg & M_RESET_RESET_BOR_BIT) != 0;
    m_reset_reason = parseResetReason((resetReg & M_RESET_RESET_REASON_MASK) >> M_RESET_RESET_REASON_POS);

    // Seed BOOST voltage cache
    uint8_t boostVout { 0 };
    rc = m_transport.Read(M_I2C_ADDRESS, M_BOOST_VOUT, &boostVout, 1, m_transport.ctx);
    if (rc < 0) {
      M_LOG_ERR("Init BOOST.VOUT read failed: %d", rc);
      return -EIO;
    }
    m_boost_millivolts = static_cast<uint16_t>(M_BOOST_VOUT_BASE_MV + (boostVout & M_BOOST_VOUT_LVL_MASK) * M_BOOST_VOUT_STEP_MV);

    // Seed LDOSW voltage cache
    uint8_t ldoVout { 0 };
    rc = m_transport.Read(M_I2C_ADDRESS, M_LDOSW_VOUT, &ldoVout, 1, m_transport.ctx);
    if (rc < 0) {
      M_LOG_ERR("Init LDOSW.VOUT read failed: %d", rc);
      return -EIO;
    }
    const uint8_t ldoLvl = ldoVout & M_LDOSW_VOUT_LVL_MASK;
    m_ldo_sw_millivolts  = static_cast<uint16_t>(M_LDOSW_VOUT_BASE_MV + ldoLvl * M_LDOSW_VOUT_STEP_MV);

    if (clearResetReason) {
      m_initialised = true; // ClearResetReason needs this to succeed
      rc            = ClearResetReason();
      if (rc < 0) {
        m_initialised = false;
        return rc;
      }
    }

    m_initialised = true;
    return 0;
  }

  // ---------- private helpers ----------

  int Npm2100::readRegister(uint8_t regAddr, uint8_t& value)
  {
    if (!m_initialised) return -ENODEV;
    int rc = m_transport.Read(M_I2C_ADDRESS, regAddr, &value, 1, m_transport.ctx);
    if (rc < 0) {
      M_LOG_ERR("Read 0x%02x failed: %d", regAddr, rc);
      return -EIO;
    }
    return 0;
  }

  int Npm2100::writeRegister(uint8_t regAddr, uint8_t value)
  {
    if (!m_initialised) return -ENODEV;
    uint8_t buf[2] { regAddr, value };
    int rc = m_transport.Write(M_I2C_ADDRESS, buf, sizeof(buf), m_transport.ctx);
    if (rc < 0) {
      M_LOG_ERR("Write 0x%02x = 0x%02x failed: %d", regAddr, value, rc);
      return -EIO;
    }
    return 0;
  }

  int Npm2100::updateRegister(uint8_t regAddr, uint8_t value, uint8_t mask)
  {
    uint8_t current { 0 };
    int rc = readRegister(regAddr, current);
    if (rc < 0) return rc;
    uint8_t merged = (current & ~mask) | (value & mask);
    return writeRegister(regAddr, merged);
  }

  int Npm2100::writeTask(uint8_t regAddr)
  {
    return writeRegister(regAddr, M_TASK_TRIGGER);
  }

  int Npm2100::readBurst(uint8_t firstReg, uint8_t* dst, size_t len)
  {
    if (!m_initialised) return -ENODEV;
    int rc = m_transport.Read(M_I2C_ADDRESS, firstReg, dst, len, m_transport.ctx);
    if (rc < 0) {
      M_LOG_ERR("ReadBurst 0x%02x len=%zu failed: %d", firstReg, len, rc);
      return -EIO;
    }
    return 0;
  }

  int Npm2100::writeBurst(uint8_t firstReg, const uint8_t* src, size_t len)
  {
    if (!m_initialised) return -ENODEV;
    if (len > M_WRITE_BURST_MAX_PAYLOAD) return -EINVAL;
    uint8_t buf[M_WRITE_BURST_BUF_SIZE];
    buf[0] = firstReg;
    std::memcpy(&buf[1], src, len);
    int rc = m_transport.Write(M_I2C_ADDRESS, buf, len + 1, m_transport.ctx);
    if (rc < 0) {
      M_LOG_ERR("WriteBurst 0x%02x len=%zu failed: %d", firstReg, len, rc);
      return -EIO;
    }
    return 0;
  }

  int Npm2100::ClearResetReason()
  {
    if (!m_initialised) return -ENODEV;
    return writeTask(M_RESET_TASKS_CLR);
  }

  int Npm2100::SoftwareReset()
  {
    if (!m_initialised) return -ENODEV;
    return writeTask(M_RESET_TASKS_RESET);
  }

  Npm2100::ResetReason Npm2100::parseResetReason(uint8_t raw)
  {
    return (raw <= 13) ? static_cast<ResetReason>(raw) : ResetReason::Unknown;
  }

  // ---------- RESET (button, scratch, sticky) ----------

  int Npm2100::ConfigureResetButton(ResetButtonPin pin, ResetButtonHoldS holdTime, bool longPressEnabled)
  {
    if (!m_initialised) return -ENODEV;
    int rc = writeRegister(M_RESET_BUTTON, longPressEnabled ? 0 : M_RESET_BUTTON_LONGPRESS_BIT);
    if (rc < 0) return rc;
    rc = writeRegister(M_RESET_PIN, (pin == ResetButtonPin::ShpHld) ? M_RESET_PIN_SHPHLD_BIT : 0);
    if (rc < 0) return rc;
    return writeRegister(M_RESET_DEBOUNCE, static_cast<uint8_t>(holdTime) & M_RESET_DEBOUNCE_TIME_MASK);
  }

  int Npm2100::SetWdResetLdoSwMode(WdResetLdoSwMode mode)
  {
    if (!m_initialised) return -ENODEV;
    return writeRegister(M_RESET_ALTCONFIG, (mode == WdResetLdoSwMode::TurnOff) ? M_RESET_ALTCONFIG_LDOSW_BIT : 0);
  }

  int Npm2100::WriteScratchA(uint8_t value)
  {
    if (!m_initialised) return -ENODEV;
    int rc = writeRegister(M_RESET_WRITE, value);
    if (rc < 0) return rc;
    return writeRegister(M_RESET_STROBE, M_TASK_TRIGGER);
  }

  int Npm2100::ReadScratchA(uint8_t& value)
  {
    return readRegister(M_RESET_READ, value);
  }

  int Npm2100::WriteScratchB(uint8_t value)
  {
    if (!m_initialised) return -ENODEV;
    return writeRegister(M_RESET_SCRATCHB, value);
  }

  int Npm2100::ReadScratchB(uint8_t& value)
  {
    return readRegister(M_RESET_SCRATCHB, value);
  }

  int Npm2100::writeStickyBits(uint8_t value)
  {
    int rc = writeRegister(M_RESET_WRITESTICKY, value);
    if (rc < 0) return rc;
    return writeRegister(M_RESET_STROBSTICKY, M_TASK_TRIGGER);
  }

  int Npm2100::ConfigureBootMonitor(bool useRegister, bool enabled)
  {
    if (!m_initialised) return -ENODEV;
    uint8_t bits = 0;
    if (useRegister) bits |= M_RESET_STICKY_BOOTMONSEL_BIT;
    if (enabled) bits |= M_RESET_STICKY_BOOTMONEN_BIT;
    uint8_t current { 0 };
    int rc = readRegister(M_RESET_READSTICKY, current);
    if (rc < 0) return rc;
    uint8_t merged = (current & M_RESET_STICKY_PWRBUTTON_BIT) | bits;
    return writeStickyBits(merged);
  }

  int Npm2100::IsBootMonitorActive(bool& active)
  {
    uint8_t reg { 0 };
    int rc = readRegister(M_RESET_SYSGDENSTATUS, reg);
    if (rc < 0) return rc;
    active = (reg & M_RESET_SYSGDENSTATUS_BOOTMON_BIT) != 0;
    return 0;
  }

  int Npm2100::DisablePowerOffButton(bool disabled)
  {
    if (!m_initialised) return -ENODEV;
    uint8_t current { 0 };
    int rc = readRegister(M_RESET_READSTICKY, current);
    if (rc < 0) return rc;
    uint8_t merged = current & ~M_RESET_STICKY_PWRBUTTON_BIT;
    if (disabled) merged |= M_RESET_STICKY_PWRBUTTON_BIT;
    return writeStickyBits(merged);
  }

  // ---------- BOOST ----------

  int Npm2100::BoostSetVoltage(uint16_t millivolts)
  {
    if (!m_initialised) return -ENODEV;

    const uint16_t maxMv = M_BOOST_VOUT_BASE_MV + M_BOOST_VOUT_LVL_MAX * M_BOOST_VOUT_STEP_MV;
    if (millivolts < M_BOOST_VOUT_BASE_MV || millivolts > maxMv) {
      M_LOG_WRN("BoostSetVoltage(%u) out of range [%u,%u]", millivolts, M_BOOST_VOUT_BASE_MV, maxMv);
      return -EINVAL;
    }

    const uint8_t lvl        = static_cast<uint8_t>((millivolts - M_BOOST_VOUT_BASE_MV + M_BOOST_VOUT_STEP_MV / 2) / M_BOOST_VOUT_STEP_MV);
    const uint16_t snappedMv = static_cast<uint16_t>(M_BOOST_VOUT_BASE_MV + lvl * M_BOOST_VOUT_STEP_MV);

    int rc = writeRegister(M_BOOST_VOUT, lvl & M_BOOST_VOUT_LVL_MASK);
    if (rc < 0) return rc;
    rc = writeRegister(M_BOOST_VOUTSEL, M_BOOST_VOUTSEL_PINREG_BIT);
    if (rc < 0) return rc;

    m_boost_millivolts = snappedMv;
    return 0;
  }

  int Npm2100::BoostSetMode(BoostMode mode)
  {
    if (!m_initialised) return -ENODEV;
    uint8_t raw { 0 };
    switch (mode) {
      case BoostMode::Auto:
        raw = M_BOOST_OPER_MODE_AUTO;
        break;
      case BoostMode::ForcedHp:
        raw = M_BOOST_OPER_MODE_HP;
        break;
      case BoostMode::ForcedLp:
        raw = M_BOOST_OPER_MODE_LP;
        break;
      case BoostMode::ForcedPt:
        raw = M_BOOST_OPER_MODE_PT;
        break;
      case BoostMode::PreventHp:
        raw = M_BOOST_OPER_MODE_NOHP;
        break;
      default:
        return -EINVAL;
    }
    return updateRegister(M_BOOST_OPER, raw, M_BOOST_OPER_MODE_MASK);
  }

  int Npm2100::BoostGetActiveMode(BoostActiveMode& mode)
  {
    uint8_t status0 { 0 };
    int rc = readRegister(M_BOOST_STATUS0, status0);
    if (rc < 0) return rc;
    const uint8_t raw = status0 & M_BOOST_STATUS0_MODE_MASK;
    if (raw > M_BOOST_STATUS0_MODE_DPS) return -EIO;
    mode = static_cast<BoostActiveMode>(raw);
    return 0;
  }

  int Npm2100::BoostIsAtTarget(bool& atTarget)
  {
    uint8_t status1 { 0 };
    int rc = readRegister(M_BOOST_STATUS1, status1);
    if (rc < 0) return rc;
    atTarget = (status1 & M_BOOST_STATUS1_VOUTLVL_BIT) != 0;
    return 0;
  }

  int Npm2100::BoostSetVbatLowThreshold(uint16_t millivolts)
  {
    if (!m_initialised) return -ENODEV;

    const uint16_t maxMv = M_BOOST_VBATMINH_BASE_MV + M_BOOST_VBATMINH_LVL_MAX * M_BOOST_VBATMINH_STEP_MV;
    if (millivolts < M_BOOST_VBATMINH_BASE_MV || millivolts > maxMv) {
      M_LOG_WRN("BoostSetVbatLowThreshold(%u) out of range [%u,%u]", millivolts, M_BOOST_VBATMINH_BASE_MV, maxMv);
      return -EINVAL;
    }
    const uint8_t lvl = static_cast<uint8_t>((millivolts - M_BOOST_VBATMINH_BASE_MV + M_BOOST_VBATMINH_STEP_MV / 2) / M_BOOST_VBATMINH_STEP_MV);

    int rc = writeRegister(M_BOOST_VBATMINH, lvl & M_BOOST_VBATMINH_LVL_MASK);
    if (rc < 0) return rc;
    return updateRegister(M_BOOST_VBATMINHSEL, M_BOOST_VBATMINHSEL_BIT, M_BOOST_VBATMINHSEL_BIT);
  }

  int Npm2100::BoostEnableVbatLowMonitor()
  {
    if (!m_initialised) return -ENODEV;
    return writeRegister(M_BOOST_CTRLSET, M_BOOST_CTRL_VBATMINSEL_BIT);
  }

  int Npm2100::BoostDisableVbatLowMonitor()
  {
    if (!m_initialised) return -ENODEV;
    return writeRegister(M_BOOST_CTRLCLR, M_BOOST_CTRL_VBATMINSEL_BIT);
  }

  // ---------- LDOSW ----------

  int Npm2100::LdoSwSetOutputMode(LdoSwOutputMode mode)
  {
    if (!m_initialised) return -ENODEV;
    uint8_t raw = (mode == LdoSwOutputMode::Ldo) ? M_LDOSW_SEL_MODE_LDO : M_LDOSW_SEL_MODE_LOADSW;
    return updateRegister(M_LDOSW_SEL, raw, M_LDOSW_SEL_MODE_MASK);
  }

  int Npm2100::LdoSwSetPowerMode(LdoSwPowerMode mode)
  {
    if (!m_initialised) return -ENODEV;
    uint8_t raw { 0 };
    switch (mode) {
      case LdoSwPowerMode::Auto:
        raw = M_LDOSW_SEL_OPER_AUTO;
        break;
      case LdoSwPowerMode::Ulp:
        raw = M_LDOSW_SEL_OPER_ULP;
        break;
      case LdoSwPowerMode::Hp:
        raw = M_LDOSW_SEL_OPER_HP;
        break;
      default:
        return -EINVAL;
    }
    return updateRegister(M_LDOSW_SEL, static_cast<uint8_t>(raw << M_LDOSW_SEL_OPER_POS), M_LDOSW_SEL_OPER_MASK);
  }

  int Npm2100::LdoSwSetVoltage(uint16_t millivolts)
  {
    if (!m_initialised) return -ENODEV;

    const uint16_t minMv = M_LDOSW_VOUT_BASE_MV + M_LDOSW_VOUT_LVL_MIN * M_LDOSW_VOUT_STEP_MV;
    const uint16_t maxMv = M_LDOSW_VOUT_BASE_MV + M_LDOSW_VOUT_LVL_MAX * M_LDOSW_VOUT_STEP_MV;
    if (millivolts < minMv || millivolts > maxMv) {
      M_LOG_WRN("LdoSwSetVoltage(%u) out of range [%u,%u]", millivolts, minMv, maxMv);
      return -EINVAL;
    }
    const uint8_t lvl        = static_cast<uint8_t>((millivolts - M_LDOSW_VOUT_BASE_MV + M_LDOSW_VOUT_STEP_MV / 2) / M_LDOSW_VOUT_STEP_MV);
    const uint16_t snappedMv = static_cast<uint16_t>(M_LDOSW_VOUT_BASE_MV + lvl * M_LDOSW_VOUT_STEP_MV);

    int rc = writeRegister(M_LDOSW_VOUT, lvl & M_LDOSW_VOUT_LVL_MASK);
    if (rc < 0) return rc;
    m_ldo_sw_millivolts = snappedMv;
    return 0;
  }

  int Npm2100::LdoSwEnable()
  {
    if (!m_initialised) return -ENODEV;
    return writeRegister(M_LDOSW_LDOSW, M_LDOSW_ENABLE_BIT);
  }

  int Npm2100::LdoSwDisable()
  {
    if (!m_initialised) return -ENODEV;
    return writeRegister(M_LDOSW_LDOSW, 0);
  }

  int Npm2100::LdoSwIsEnabled(bool& enabled)
  {
    uint8_t reg { 0 };
    int rc = readRegister(M_LDOSW_LDOSW, reg);
    if (rc < 0) return rc;
    enabled = (reg & M_LDOSW_ENABLE_BIT) != 0;
    return 0;
  }

  int Npm2100::LdoSwReadOcpFault(bool& ocpActive)
  {
    uint8_t status { 0 };
    int rc = readRegister(M_LDOSW_STATUS, status);
    if (rc < 0) return rc;
    ocpActive = (status & M_LDOSW_STATUS_OCP_BIT) != 0;
    return 0;
  }

  // ---------- ADC ----------

  int Npm2100::adcSingleShot(uint8_t mode, uint8_t readyBit, uint8_t resultReg, uint8_t& raw)
  {
    if (!m_initialised) return -ENODEV;

    int rc = writeRegister(M_MAIN_EVENTS_ADC_CLR, readyBit);
    if (rc < 0) return rc;

    rc = updateRegister(M_ADC_CONFIG, mode, M_ADC_CONFIG_MODE_MASK);
    if (rc < 0) return rc;

    rc = writeRegister(M_ADC_TASKS_ADC, M_ADC_TASKS_ADC_CONV_BIT);
    if (rc < 0) return rc;

    bool ready = false;
    for (uint8_t i = 0; i < M_ADC_MAX_POLLS; ++i) {
      uint8_t ev { 0 };
      rc = readRegister(M_MAIN_EVENTS_ADC_SET, ev);
      if (rc < 0) return rc;
      if (ev & readyBit) {
        ready = true;
        break;
      }
    }
    if (!ready) {
      M_LOG_ERR("ADC conversion timed out (mode=%u)!", mode);
      return -ETIMEDOUT;
    }

    rc = readRegister(resultReg, raw);
    if (rc < 0) return rc;
    return writeRegister(M_MAIN_EVENTS_ADC_CLR, readyBit);
  }

  int Npm2100::AdcReadVbat(float& volts)
  {
    uint8_t raw { 0 };
    int rc = adcSingleShot(M_ADC_CONFIG_MODE_INSVBAT, M_EVENTS_ADC_VBATRDY_BIT, M_ADC_READVBAT, raw);
    if (rc < 0) return rc;
    volts = static_cast<float>(raw) * M_ADC_VBAT_FULL_SCALE_V / M_ADC_DENOMINATOR;
    return 0;
  }

  int Npm2100::AdcReadVout(float& volts)
  {
    uint8_t raw { 0 };
    int rc = adcSingleShot(M_ADC_CONFIG_MODE_VOUT, M_EVENTS_ADC_VOUTRDY_BIT, M_ADC_READVOUT, raw);
    if (rc < 0) return rc;
    volts = M_ADC_VOUT_BASE_V + static_cast<float>(raw) * M_ADC_VOUT_FULL_SCALE_V / M_ADC_DENOMINATOR;
    return 0;
  }

  int Npm2100::AdcReadDieTemp(float& celsius)
  {
    uint8_t raw { 0 };
    int rc = adcSingleShot(M_ADC_CONFIG_MODE_DIETEMP, M_EVENTS_ADC_DIETRDY_BIT, M_ADC_READTEMP, raw);
    if (rc < 0) return rc;
    celsius = M_ADC_TEMP_OFFSET_C - M_ADC_TEMP_SLOPE_C * static_cast<float>(raw);
    return 0;
  }

  // ---------- TIMER ----------

  int Npm2100::TimerStop()
  {
    if (!m_initialised) return -ENODEV;
    return writeRegister(M_TIMER_TASKS_STOP, M_TASK_TRIGGER);
  }

  int Npm2100::TimerSetMode(TimerMode mode)
  {
    if (!m_initialised) return -ENODEV;
    uint8_t raw { 0 };
    switch (mode) {
      case TimerMode::GeneralPurpose:
        raw = M_TIMER_CONFIG_MODE_GENPURP;
        break;
      case TimerMode::WatchdogReset:
        raw = M_TIMER_CONFIG_MODE_WDRST;
        break;
      case TimerMode::WatchdogPwrCyc:
        raw = M_TIMER_CONFIG_MODE_WDPWRC;
        break;
      case TimerMode::Wakeup:
        raw = M_TIMER_CONFIG_MODE_WKUP;
        break;
      default:
        return -EINVAL;
    }
    return updateRegister(M_TIMER_CONFIG, raw, M_TIMER_CONFIG_MODE_MASK);
  }

  int Npm2100::TimerSetDurationMs(uint32_t milliseconds)
  {
    if (!m_initialised) return -ENODEV;

    if (milliseconds < 16) {
      M_LOG_WRN("TimerSetDurationMs(%u): min 16 ms!", milliseconds);
      return -EINVAL;
    }
    // ticks = round(ms / 15.625) using integer math: (ms * 1000 + 7812) / 15625.
    uint64_t ticks = (static_cast<uint64_t>(milliseconds) * 1000u + 7812u) / 15625u;
    if (ticks == 0) ticks = 1;
    uint64_t value = ticks - 1;
    if (value > M_TIMER_MAX_VALUE) {
      M_LOG_WRN("TimerSetDurationMs(%u): exceeds max ~262e6 ms!", milliseconds);
      return -EINVAL;
    }

    uint8_t hi  = static_cast<uint8_t>((value >> 16) & 0xFF);
    uint8_t mid = static_cast<uint8_t>((value >> 8) & 0xFF);
    uint8_t lo  = static_cast<uint8_t>(value & 0xFF);

    uint8_t buf[3] { hi, mid, lo };
    return writeBurst(M_TIMER_TARGETHI, buf, 3);
  }

  int Npm2100::TimerStart()
  {
    if (!m_initialised) return -ENODEV;
    return writeRegister(M_TIMER_TASKS_START, M_TASK_TRIGGER);
  }

  int Npm2100::TimerKickWatchdog()
  {
    if (!m_initialised) return -ENODEV;
    return writeRegister(M_TIMER_TASKS_KICK, M_TASK_TRIGGER);
  }

  int Npm2100::TimerIsExpired(bool& expired)
  {
    uint8_t ev { 0 };
    int rc = readRegister(M_MAIN_EVENTS_SYSTEM_SET, ev);
    if (rc < 0) return rc;
    expired = (ev & M_SYSTEM_TIMER_BIT) != 0;
    return 0;
  }

  int Npm2100::TimerClearExpiredEvent()
  {
    if (!m_initialised) return -ENODEV;
    return writeRegister(M_MAIN_EVENTS_SYSTEM_CLR, M_SYSTEM_TIMER_BIT);
  }

  int Npm2100::TimerEnableInterrupt(bool preWarn, bool freeEvent)
  {
    if (!m_initialised) return -ENODEV;
    uint8_t bits = M_SYSTEM_TIMER_BIT;
    if (preWarn) bits |= M_SYSTEM_TIMERPREWARN_BIT;
    if (freeEvent) bits |= M_SYSTEM_TIMERFREE_BIT;
    return writeRegister(M_MAIN_INTEN_SYSTEM_SET, bits);
  }

  int Npm2100::TimerDisableInterrupt()
  {
    if (!m_initialised) return -ENODEV;
    const uint8_t allTimerBits = M_SYSTEM_TIMER_BIT | M_SYSTEM_TIMERPREWARN_BIT | M_SYSTEM_TIMERFREE_BIT;
    return writeRegister(M_MAIN_INTEN_SYSTEM_CLR, allTimerBits);
  }

  // ---------- SHIP & Break-to-Wake ----------

  int Npm2100::ShipEnter()
  {
    if (!m_initialised) return -ENODEV;
    return writeRegister(M_SHIP_TASKS_SHIP, M_TASK_TRIGGER);
  }

  int Npm2100::ShipSetWakeEdge(ShipWakeEdge edge)
  {
    if (!m_initialised) return -ENODEV;
    return updateRegister(M_SHIP_WAKEUP, (edge == ShipWakeEdge::Rising) ? M_SHIP_WAKEUP_EDGE_BIT : 0, M_SHIP_WAKEUP_EDGE_BIT);
  }

  int Npm2100::ShipConfigureBreakToWake(ShpHldResistor resistor, ShpHldPullCurrent current, bool weakPullUpEnabled)
  {
    if (!m_initialised) return -ENODEV;
    uint8_t reg = static_cast<uint8_t>(resistor) & M_SHIP_SHPHLD_RESISTOR_MASK;
    reg |= (static_cast<uint8_t>(current) << M_SHIP_SHPHLD_CURR_POS) & M_SHIP_SHPHLD_CURR_MASK;
    if (weakPullUpEnabled) reg |= M_SHIP_SHPHLD_PULL_BIT;
    return writeRegister(M_SHIP_SHPHLD, reg);
  }

  int Npm2100::ShpHldEnableHibernateWake()
  {
    if (!m_initialised) return -ENODEV;
    return updateRegister(M_SHIP_WAKEUP, 0, M_SHIP_WAKEUP_HIBERNATE_BIT);
  }

  int Npm2100::ShpHldDisableHibernateWake()
  {
    if (!m_initialised) return -ENODEV;
    return updateRegister(M_SHIP_WAKEUP, M_SHIP_WAKEUP_HIBERNATE_BIT, M_SHIP_WAKEUP_HIBERNATE_BIT);
  }

  // ---------- HIBERNATE ----------

  int Npm2100::HibernateEnter()
  {
    if (!m_initialised) return -ENODEV;
    return writeRegister(M_HIB_TASKS_HIBER, M_TASK_TRIGGER);
  }

  int Npm2100::HibernateEnterPassThrough()
  {
    if (!m_initialised) return -ENODEV;
    return writeRegister(M_HIB_TASKS_HIBERPT, M_TASK_TRIGGER);
  }

  int Npm2100::HibernateSetWakeDebounce(HibernateDebounceMs setting)
  {
    if (!m_initialised) return -ENODEV;
    uint8_t value = M_HIB_DEBOUNCE_ENABLE_BIT | ((static_cast<uint8_t>(setting) << M_HIB_DEBOUNCE_TIME_POS) & M_HIB_DEBOUNCE_TIME_MASK);
    return writeRegister(M_HIB_DEBOUNCE, value);
  }

  int Npm2100::HibernateDisableWakeDebounce()
  {
    if (!m_initialised) return -ENODEV;
    return updateRegister(M_HIB_DEBOUNCE, 0, M_HIB_DEBOUNCE_ENABLE_BIT);
  }

  // ---------- GPIO ----------

  int Npm2100::GpioConfigure(GpioPin pin, const GpioConfig& cfg)
  {
    if (!m_initialised) return -ENODEV;
    uint8_t v = 0;
    if (cfg.inputEnabled) v |= M_GPIO_CFG_INPUT_BIT;
    if (cfg.outputEnabled) v |= M_GPIO_CFG_OUTPUT_BIT;
    if (cfg.openDrain) v |= M_GPIO_CFG_OPENDRAIN_BIT;
    if (cfg.pullDown) v |= M_GPIO_CFG_PULLDOWN_BIT;
    if (cfg.pullUp) v |= M_GPIO_CFG_PULLUP_BIT;
    if (cfg.drive == GpioDrive::Strong) v |= M_GPIO_CFG_DRIVE_BIT;
    if (cfg.debounce) v |= M_GPIO_CFG_DEBOUNCE_BIT;
    const uint8_t reg = (pin == GpioPin::Gpio0) ? M_GPIO_CONFIG0 : M_GPIO_CONFIG1;
    return writeRegister(reg, v);
  }

  int Npm2100::GpioSetUsage(GpioPin pin, GpioUsage usage)
  {
    if (!m_initialised) return -ENODEV;
    uint8_t raw { 0 };
    switch (usage) {
      case GpioUsage::Gpio:
        raw = M_GPIO_USAGE_GPIO;
        break;
      case GpioUsage::InterruptLo:
        raw = M_GPIO_USAGE_INTLO;
        break;
      case GpioUsage::InterruptHi:
        raw = M_GPIO_USAGE_INTHI;
        break;
      default:
        return -EINVAL;
    }
    const uint8_t reg = (pin == GpioPin::Gpio0) ? M_GPIO_USAGE0 : M_GPIO_USAGE1;
    return writeRegister(reg, raw);
  }

  int Npm2100::GpioWrite(GpioPin pin, bool high)
  {
    if (!m_initialised) return -ENODEV;
    const uint8_t reg = (pin == GpioPin::Gpio0) ? M_GPIO_OUTPUT0 : M_GPIO_OUTPUT1;
    return writeRegister(reg, high ? 0x01 : 0x00);
  }

  int Npm2100::GpioRead(GpioPin pin, bool& high)
  {
    uint8_t bits { 0 };
    int rc = readRegister(M_GPIO_READ, bits);
    if (rc < 0) return rc;
    const uint8_t mask = (pin == GpioPin::Gpio0) ? 0x01 : 0x02;
    high               = (bits & mask) != 0;
    return 0;
  }

  int Npm2100::GpioReadAll(uint8_t& bits)
  {
    return readRegister(M_GPIO_READ, bits);
  }

  // ---------- Live status & die-temp monitoring ----------

  int Npm2100::IsShpHldHigh(bool& high)
  {
    uint8_t s { 0 };
    int rc = readRegister(M_MAIN_STATUS, s);
    if (rc < 0) return rc;
    high = (s & M_MAIN_STATUS_SHPHLD_BIT) != 0;
    return 0;
  }

  int Npm2100::IsPgResetHigh(bool& high)
  {
    uint8_t s { 0 };
    int rc = readRegister(M_MAIN_STATUS, s);
    if (rc < 0) return rc;
    high = (s & M_MAIN_STATUS_PGRESET_BIT) != 0;
    return 0;
  }

  int Npm2100::IsDieTempWarning(bool& warning)
  {
    uint8_t s { 0 };
    int rc = readRegister(M_MAIN_STATUS, s);
    if (rc < 0) return rc;
    warning = (s & M_MAIN_STATUS_DIETEMP_BIT) != 0;
    return 0;
  }

  int Npm2100::EnableDieTempMonitoring(bool whileBoostInHpOnly)
  {
    if (!m_initialised) return -ENODEV;
    uint8_t bits = M_MAIN_REQUEST_DIETEMP_BIT;
    if (whileBoostInHpOnly) bits |= M_MAIN_REQUEST_DIETEMPENA_BIT;
    return writeRegister(M_MAIN_REQUESTSET, bits);
  }

  int Npm2100::DisableDieTempMonitoring()
  {
    if (!m_initialised) return -ENODEV;
    return writeRegister(M_MAIN_REQUESTCLR, M_MAIN_REQUEST_DIETEMP_BIT | M_MAIN_REQUEST_DIETEMPENA_BIT);
  }

  // ---------- Events & Interrupts (per-group) ----------

  int Npm2100::GetSystemEventsPending(uint8_t& bits)
  {
    return eventGet(M_MAIN_EVENTS_SYSTEM_SET, bits);
  }
  int Npm2100::ClearSystemEvents(uint8_t bits)
  {
    return eventClear(M_MAIN_EVENTS_SYSTEM_CLR, bits);
  }
  int Npm2100::EnableSystemInterrupts(uint8_t bits)
  {
    return eventEnable(M_MAIN_INTEN_SYSTEM_SET, bits);
  }
  int Npm2100::DisableSystemInterrupts(uint8_t bits)
  {
    return eventDisable(M_MAIN_INTEN_SYSTEM_CLR, bits);
  }

  int Npm2100::GetAdcEventsPending(uint8_t& bits)
  {
    return eventGet(M_MAIN_EVENTS_ADC_SET, bits);
  }
  int Npm2100::ClearAdcEvents(uint8_t bits)
  {
    return eventClear(M_MAIN_EVENTS_ADC_CLR, bits);
  }
  int Npm2100::EnableAdcInterrupts(uint8_t bits)
  {
    return eventEnable(M_MAIN_INTEN_ADC_SET, bits);
  }
  int Npm2100::DisableAdcInterrupts(uint8_t bits)
  {
    return eventDisable(M_MAIN_INTEN_ADC_CLR, bits);
  }

  int Npm2100::GetGpioEventsPending(uint8_t& bits)
  {
    return eventGet(M_MAIN_EVENTS_GPIO_SET, bits);
  }
  int Npm2100::ClearGpioEvents(uint8_t bits)
  {
    return eventClear(M_MAIN_EVENTS_GPIO_CLR, bits);
  }
  int Npm2100::EnableGpioInterrupts(uint8_t bits)
  {
    return eventEnable(M_MAIN_INTEN_GPIO_SET, bits);
  }
  int Npm2100::DisableGpioInterrupts(uint8_t bits)
  {
    return eventDisable(M_MAIN_INTEN_GPIO_CLR, bits);
  }

  int Npm2100::GetBoostEventsPending(uint8_t& bits)
  {
    return eventGet(M_MAIN_EVENTS_BOOST_SET, bits);
  }
  int Npm2100::ClearBoostEvents(uint8_t bits)
  {
    return eventClear(M_MAIN_EVENTS_BOOST_CLR, bits);
  }
  int Npm2100::EnableBoostInterrupts(uint8_t bits)
  {
    return eventEnable(M_MAIN_INTEN_BOOST_SET, bits);
  }
  int Npm2100::DisableBoostInterrupts(uint8_t bits)
  {
    return eventDisable(M_MAIN_INTEN_BOOST_CLR, bits);
  }

  int Npm2100::GetLdoSwEventsPending(uint8_t& bits)
  {
    return eventGet(M_MAIN_EVENTS_LDOSW_SET, bits);
  }
  int Npm2100::ClearLdoSwEvents(uint8_t bits)
  {
    return eventClear(M_MAIN_EVENTS_LDOSW_CLR, bits);
  }
  int Npm2100::EnableLdoSwInterrupts(uint8_t bits)
  {
    return eventEnable(M_MAIN_INTEN_LDOSW_SET, bits);
  }
  int Npm2100::DisableLdoSwInterrupts(uint8_t bits)
  {
    return eventDisable(M_MAIN_INTEN_LDOSW_CLR, bits);
  }

  int Npm2100::ClearAllPendingEvents()
  {
    if (!m_initialised) return -ENODEV;
    int rc;
    rc = writeRegister(M_MAIN_EVENTS_SYSTEM_CLR, 0xFF);
    if (rc < 0) return rc;
    rc = writeRegister(M_MAIN_EVENTS_ADC_CLR, 0xFF);
    if (rc < 0) return rc;
    rc = writeRegister(M_MAIN_EVENTS_GPIO_CLR, 0xFF);
    if (rc < 0) return rc;
    rc = writeRegister(M_MAIN_EVENTS_BOOST_CLR, 0xFF);
    if (rc < 0) return rc;
    return writeRegister(M_MAIN_EVENTS_LDOSW_CLR, 0xFF);
  }

} // namespace alc
