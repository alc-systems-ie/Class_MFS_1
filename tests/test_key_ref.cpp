#include <cassert>
#include <cerrno>
#include <cstdio>

#include "access_vectors.hpp"
#include "crypto.hpp"
#include "host_kmu.hpp"
#include "kmu_slots.hpp"

void run_key_ref_tests()
{
  using namespace alc;

  // HMAC-SHA256 of the day-key message under the 00..1F secret.
  constexpr uint8_t M_MESSAGE[] { 0x4D, 0x46, 0x53, 0x31, 0x01, 0x00, 0x01, 0x02 };
  constexpr uint8_t M_EXPECTED[crypto::M_HMAC_SHA256_BYTES] { 0xAD, 0x84, 0x79, 0x12, 0xAA, 0x46, 0x96, 0x7B, 0xAF, 0x7B, 0x87,
                                                              0x38, 0xA5, 0xDC, 0x95, 0xD2, 0xEB, 0x7F, 0x40, 0xAF, 0x5A, 0x6D,
                                                              0x3A, 0x76, 0x10, 0x18, 0x63, 0x46, 0x5A, 0xFD, 0x77, 0x86 };
  constexpr uint8_t M_UNTOUCHED { 0x5A };

  uint8_t mac[crypto::M_HMAC_SHA256_BYTES] {};

  crypto::host_kmu::Clear();

  // A raw reference behaves as the raw-pointer overload.
  assert(crypto::HmacSha256(crypto::KeyRef::Raw(access::vectors::M_SECRET, sizeof(access::vectors::M_SECRET)), M_MESSAGE, sizeof(M_MESSAGE), mac) ==
         0);
  assert(crypto::ConstantTimeEqual(mac, M_EXPECTED, sizeof(mac)));
  assert(!crypto::KeyRef::Raw(access::vectors::M_SECRET, sizeof(access::vectors::M_SECRET)).IsKmu());

  // A provisioned KMU slot gives the same MAC.
  crypto::host_kmu::Provision(kmu::M_SECRET_SLOT, access::vectors::M_SECRET, sizeof(access::vectors::M_SECRET));
  for (uint8_t& byte : mac) {
    byte = 0;
  }
  assert(crypto::KeyRef::Kmu(kmu::M_SECRET_SLOT).IsKmu());
  assert(crypto::HmacSha256(crypto::KeyRef::Kmu(kmu::M_SECRET_SLOT), M_MESSAGE, sizeof(M_MESSAGE), mac) == 0);
  assert(crypto::ConstantTimeEqual(mac, M_EXPECTED, sizeof(mac)));

  // An unprovisioned slot is -ENOKEY and leaves the output alone.
  for (uint8_t& byte : mac) {
    byte = M_UNTOUCHED;
  }
  assert(crypto::HmacSha256(crypto::KeyRef::Kmu(kmu::M_PROVISION_SLOT), M_MESSAGE, sizeof(M_MESSAGE), mac) == -ENOKEY);
  for (const uint8_t byte : mac) {
    assert(byte == M_UNTOUCHED);
  }

  // Clear forgets provisioned keys.
  crypto::host_kmu::Clear();
  assert(crypto::HmacSha256(crypto::KeyRef::Kmu(kmu::M_SECRET_SLOT), M_MESSAGE, sizeof(M_MESSAGE), mac) == -ENOKEY);

  // A raw reference with no bytes, or no length, is a bug: both overloads refuse
  // it with -EINVAL and leave the output alone, rather than MAC under an empty key.
  {
    constexpr uint8_t M_ONE_KEY_BYTE[] { 0x01 };

    for (uint8_t& byte : mac) {
      byte = M_UNTOUCHED;
    }
    assert(crypto::HmacSha256(crypto::KeyRef::Raw(nullptr, 0), M_MESSAGE, sizeof(M_MESSAGE), mac) == -EINVAL);
    assert(crypto::HmacSha256(crypto::KeyRef::Raw(nullptr, sizeof(access::vectors::M_SECRET)), M_MESSAGE, sizeof(M_MESSAGE), mac) == -EINVAL);
    assert(crypto::HmacSha256(crypto::KeyRef::Raw(M_ONE_KEY_BYTE, 0), M_MESSAGE, sizeof(M_MESSAGE), mac) == -EINVAL);
    assert(crypto::HmacSha256(nullptr, 0, M_MESSAGE, sizeof(M_MESSAGE), mac) == -EINVAL);
    assert(crypto::HmacSha256(nullptr, sizeof(access::vectors::M_SECRET), M_MESSAGE, sizeof(M_MESSAGE), mac) == -EINVAL);
    assert(crypto::HmacSha256(M_ONE_KEY_BYTE, 0, M_MESSAGE, sizeof(M_MESSAGE), mac) == -EINVAL);
    for (const uint8_t byte : mac) {
      assert(byte == M_UNTOUCHED);
    }
  }

  // A 256-bit key takes two slots.
  assert(kmu::M_SECRET_SLOT + 2 == kmu::M_PROVISION_SLOT);

  printf("key_ref tests passed\n");
}
