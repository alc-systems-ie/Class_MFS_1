#pragma once

// HOST-ONLY stand-in for the nRF54L KMU, used by tests/crypto_openssl.cpp. Never
// included from src/ or the firmware build.

#include <cstddef>
#include <cstdint>

namespace alc::crypto::host_kmu
{

  /** @brief Place a key in the simulated KMU under the first slot of its pair. */
  void Provision(uint8_t slot, const uint8_t* key, size_t length);

  /** @brief Erase every simulated slot. */
  void Clear();

}
