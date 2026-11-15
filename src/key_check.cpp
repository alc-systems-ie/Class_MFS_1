#include <cerrno>

#include "key_check.hpp"

namespace alc::key_check
{

  namespace
  {
    // The all-zero 256-bit key: what an erased or never-written key would hold.
    constexpr uint8_t M_ZERO_KEY[crypto::M_HMAC_SHA256_BYTES] {};

    // Every key failure is -ENOKEY to the caller: absent, empty or unusable alike
    // mean the device must not accept commands.
    int checkMac(const crypto::KeyRef& key, uint8_t* mac)
    {
      int result { crypto::HmacSha256(key, M_CHECK_MESSAGE, sizeof(M_CHECK_MESSAGE), mac) };

      if (result < 0) { return -ENOKEY; }
      return 0;
    }
  }

  int CheckLongTermKeys(const crypto::KeyRef& secret, const crypto::KeyRef& provisionKey)
  {
    uint8_t secretMac[crypto::M_HMAC_SHA256_BYTES] {};
    uint8_t provisionMac[crypto::M_HMAC_SHA256_BYTES] {};
    uint8_t zeroMac[crypto::M_HMAC_SHA256_BYTES] {};
    int result { checkMac(secret, secretMac) };

    if (result < 0) { return result; }
    result = checkMac(provisionKey, provisionMac);
    if (result < 0) { return result; }

    // The zero reference is a raw key, so a failure here is a backend fault, not a missing key.
    result = crypto::HmacSha256(crypto::KeyRef::Raw(M_ZERO_KEY, sizeof(M_ZERO_KEY)), M_CHECK_MESSAGE, sizeof(M_CHECK_MESSAGE), zeroMac);
    if (result < 0) { return result; }

    // Equal keys would let a provisioner mint day keys; a zero key is no key at all.
    if (crypto::ConstantTimeEqual(secretMac, provisionMac, sizeof(secretMac))) { return -EINVAL; }
    if (crypto::ConstantTimeEqual(secretMac, zeroMac, sizeof(secretMac))) { return -EINVAL; }
    if (crypto::ConstantTimeEqual(provisionMac, zeroMac, sizeof(provisionMac))) { return -EINVAL; }
    return 0;
  }

}
