#pragma once

#include <cstdint>

namespace alc::kmu
{

  // KMU slot numbers for the two long-term HMAC keys. Each 256-bit key occupies
  // two consecutive slots. tools read these two lines with sed, so keep the
  // spelling exactly as written.
  constexpr uint8_t M_SECRET_SLOT { 100 };
  constexpr uint8_t M_PROVISION_SLOT { 102 };

}
