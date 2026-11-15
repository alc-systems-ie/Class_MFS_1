#pragma once

#include <cstdint>

#include "crypto.hpp"

namespace alc::key_check
{

  // Boot self-check of the two long-term keys (design 2026-11-15 section 5.3).
  // The firmware never reads a KMU key, so it checks them by what they compute:
  // HMAC-SHA256 of a fixed message under each key, and under an all-zero key.

  /**
   * @brief The check message: ASCII "MFS-KCV" followed by domain byte 0x07.
   *
   * 0x07 is distinct from every protocol label (0x02-0x06, docs/tan-scheme.md
   * section 3 and the FIRE amendment), so a check MAC is never a protocol MAC.
   */
  constexpr uint8_t M_CHECK_MESSAGE[] { 'M', 'F', 'S', '-', 'K', 'C', 'V', 0x07 };

  /**
   * @brief Check that both long-term keys are present, distinct and not all zero.
   *
   * Returns codes only and logs nothing: nothing derived from a key is ever logged.
   *
   * @return 0; -ENOKEY if either key cannot be used; -EINVAL if equal or either equals the all-zero key;
   *         another negative errno if the backend cannot compute the all-zero reference.
   */
  int CheckLongTermKeys(const crypto::KeyRef& secret, const crypto::KeyRef& provisionKey);

}
