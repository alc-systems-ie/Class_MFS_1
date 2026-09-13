#pragma once

#include <cstddef>
#include <cstdint>

namespace alc::credentials
{

  /**
   * @brief Parse exactly `length` bytes from a hex string of exactly 2 * length digits.
   *
   * Strict on purpose. A short or malformed secret must fail loudly at boot rather
   * than silently become a key of zeros that every build shares.
   *
   * @return True only if the whole string was consumed and every digit was hex.
   */
  bool ParseHexBytes(const char* hex, uint8_t* out, size_t length);

  /** @brief Parse an 8-digit hex device ID, e.g. "4D465331". */
  bool ParseDeviceId(const char* hex, uint32_t& deviceId);

  /** @brief True if every byte is zero - a secret nobody filled in. */
  bool IsAllZero(const uint8_t* bytes, size_t length);

}
