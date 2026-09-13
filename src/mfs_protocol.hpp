#pragma once

#include <cstdint>

#include "mfs_protocol_tables.hpp"

namespace alc::protocol
{

  // Wire format - see docs/superpowers/specs/2026-09-12-app-control-design.md
  // section 4. Byte positions are ON-AIR order, which is the 128-bit service
  // UUID transmitted least-significant byte first. The app builds its UUID
  // string as this sequence REVERSED.
  constexpr uint8_t M_UUID_BYTES { 16 };

  // On air: rotating ID | ciphertext | CCM tag. Only the rotating ID is in the
  // clear, and it is unpredictable without the day key.
  constexpr uint8_t M_OFFSET_ROTATING_ID { 0 };
  constexpr uint8_t M_ROTATING_ID_BYTES { 4 };
  constexpr uint8_t M_OFFSET_CIPHERTEXT { 4 };
  constexpr uint8_t M_PLAINTEXT_BYTES { 8 };
  constexpr uint8_t M_OFFSET_TAG { 12 };
  constexpr uint8_t M_TAG_BYTES { 4 };

  // Never transmitted. Both sides supply it as CCM associated data, so a payload
  // built for another protocol version fails authentication rather than parsing.
  constexpr uint8_t M_PROTOCOL_VERSION { 0x02 };

  // Plaintext layout, after decryption.
  constexpr uint8_t M_PT_ARM_DELAY { 0 };        // bit 0 arm, bits 1-7 delay code
  constexpr uint8_t M_PT_ACTIVATIONS_MODE { 1 }; // bits 0-3 activations - 1, bits 4-5 mode
  constexpr uint8_t M_PT_COOLDOWN { 2 };
  constexpr uint8_t M_PT_SENSITIVITY { 3 };
  constexpr uint8_t M_PT_MINUTE { 4 }; // uint16 LE, bits 0-10 UTC minute of day
  // Plaintext bytes 6-7 are per-variant extension space. MFS_1 MUST IGNORE THEM
  // and must never require them to be zero.

  constexpr uint8_t M_ARM_BIT { 0x01 };
  constexpr uint8_t M_DELAY_SHIFT { 1 };
  constexpr uint8_t M_DELAY_MASK { 0x7F };
  constexpr uint8_t M_ACTIVATIONS_MASK { 0x0F };
  constexpr uint8_t M_MODE_SHIFT { 4 };
  constexpr uint8_t M_MODE_MASK { 0x03 };
  constexpr uint16_t M_MINUTE_MASK { 0x07FF };
  constexpr uint16_t M_MINUTES_PER_DAY { 1440 };

  constexpr uint8_t M_ACTIVATIONS_MIN { 1 };
  constexpr uint8_t M_ACTIVATIONS_MAX { 16 };

  /**
   * @brief What the device does when the activation count is reached.
   *
   * Report and ReportAndTrigger make the device ADVERTISE, which is an exception
   * to the standing rule in CLAUDE.md that it never does. They are for use only
   * when absolutely necessary - advertising forfeits covertness. Only a slot-0
   * (Network Manager) command may change the mode.
   */
  enum class Mode : uint8_t {
    TriggerOnly      = 0, ///< Default. Fires the output, emits nothing.
    ReportAndTrigger = 1, ///< Broadcasts immediately before firing.
    ReportOnly       = 2, ///< Broadcasts, never fires.
    Reserved         = 3, ///< Rejected.
  };

  /** @brief A decrypted, decoded command. Byte encodings not yet resolved. */
  struct Command
  {
      bool armActive { false };
      uint8_t delayCode { 0 };
      uint8_t activations { 1 };
      Mode mode { Mode::TriggerOnly };
      uint8_t cooldownByte { 0 };
      uint8_t sensitivityByte { 0 };
      uint16_t minuteOfDay { 0 };
  };

  /**
   * @brief Decode an 8-byte plaintext.
   *
   * @param plaintext Exactly M_PLAINTEXT_BYTES bytes.
   * @param out       Populated only on success.
   * @return False for a reserved mode or a minute outside 0-1439.
   */
  bool DecodeCommand(const uint8_t* plaintext, Command& out);

  /** @brief Encode a command into M_PLAINTEXT_BYTES bytes. Extension bytes are zero. */
  void EncodeCommand(const Command& command, uint8_t* plaintext);

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
