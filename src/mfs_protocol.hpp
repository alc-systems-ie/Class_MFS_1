#pragma once

#include <cstdint>

#include "mfs_protocol_tables.hpp"

namespace alc::protocol
{

  constexpr uint8_t M_DELAY_MASK { 0x7F };

  /** @brief Sensitivity byte to ADXL367 THRESH_ACT, in 0.25 mg LSB. */
  inline uint16_t SensitivityToThresholdLsb(uint8_t sensitivityByte)
  {
    return M_THRESHOLD_TABLE[sensitivityByte];
  }

  /** @brief Cooldown byte to seconds. Zero means no cooldown. */
  inline uint16_t CooldownToSeconds(uint8_t cooldownByte)
  {
    return M_COOLDOWN_TABLE[cooldownByte];
  }

  /** @brief Delay code (7 bits) to seconds. Zero means fire immediately. */
  inline uint16_t DelayToSeconds(uint8_t delayCode)
  {
    return M_DELAY_TABLE[delayCode & M_DELAY_MASK];
  }

}
