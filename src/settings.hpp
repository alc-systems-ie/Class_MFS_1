#pragma once

#include <cstdint>

#include "mfs_protocol.hpp"

namespace alc
{

  /**
   * @brief The engineer-settable parameters, NVS-backed.
   *
   * Stored as the raw wire bytes rather than the converted values, so the table
   * lookup stays the single place a byte becomes a physical quantity.
   */
  class Settings
  {
    public:
      Settings();

      /** @brief Load from NVS, leaving defaults in place if nothing is stored. */
      int Load();

      /**
       * @brief Adopt the parameters from an accepted command and persist if they changed.
       *
       * @param allowModeChange True only for a slot-0 (Network Manager) command.
       *        Otherwise the command's mode field is IGNORED and the stored mode
       *        kept - an engineer's key never confers the power to set it.
       * @return True if anything changed. **Only writes NVS on change.**
       */
      bool ApplyFrom(const protocol::Command& command, bool allowModeChange);

      uint8_t Activations() const { return m_activations; }
      uint16_t ThresholdLsb() const { return protocol::SensitivityToThresholdLsb(m_sensitivity_byte); }
      uint16_t CooldownSeconds() const { return protocol::CooldownToSeconds(m_cooldown_byte); }
      uint8_t DelayCode() const { return m_delay_code; }
      uint16_t DelaySeconds() const { return protocol::DelayToSeconds(m_delay_code); }
      protocol::Mode OperatingMode() const { return m_mode; }

    private:
      int save();

      uint8_t m_activations;
      uint8_t m_cooldown_byte;
      uint8_t m_sensitivity_byte;
      uint8_t m_delay_code;
      protocol::Mode m_mode;
  };

}
