#include <cassert>
#include <cerrno>
#include <cstdio>
#include <initializer_list>

#include "access_keys.hpp"
#include "access_vectors.hpp"
#include "report_keys.hpp"
#include "crypto.hpp"
#include "host_kmu.hpp"
#include "key_check.hpp"
#include "kmu_slots.hpp"

void run_key_check_tests()
{
  using namespace alc;

  constexpr uint8_t M_ZERO_KEY[access::M_SECRET_BYTES] {};
  constexpr uint8_t M_EXPECTED_MESSAGE[] { 'M', 'F', 'S', '-', 'K', 'C', 'V', 0x07 };

  const crypto::KeyRef secret { crypto::KeyRef::Kmu(kmu::M_SECRET_SLOT) };
  const crypto::KeyRef provisionKey { crypto::KeyRef::Kmu(kmu::M_PROVISION_SLOT) };

  // The check message is exactly "MFS-KCV" 0x07, and its domain byte is not a
  // protocol label, so a check MAC can never equal a protocol MAC.
  static_assert(sizeof(key_check::M_CHECK_MESSAGE) == sizeof(M_EXPECTED_MESSAGE));
  for (size_t index = 0; index < sizeof(M_EXPECTED_MESSAGE); index++) {
    assert(key_check::M_CHECK_MESSAGE[index] == M_EXPECTED_MESSAGE[index]);
  }
  for (const uint8_t label : { access::M_LABEL_DAY_KEY, access::M_LABEL_ENC_KEY, access::M_LABEL_ROTATING_ID, access::M_LABEL_TIME_SYNC,
                               report::M_LABEL_REPORT_KEY, report::M_LABEL_REPORT_ENC_KEY, report::M_LABEL_REPORT_ID }) {
    assert(key_check::M_CHECK_MESSAGE[sizeof(key_check::M_CHECK_MESSAGE) - 1] != label);
  }

  // Two distinct, provisioned keys pass.
  crypto::host_kmu::Clear();
  crypto::host_kmu::Provision(kmu::M_SECRET_SLOT, access::vectors::M_SECRET, sizeof(access::vectors::M_SECRET));
  crypto::host_kmu::Provision(kmu::M_PROVISION_SLOT, access::vectors::M_PROVISION_KEY, sizeof(access::vectors::M_PROVISION_KEY));
  assert(key_check::CheckLongTermKeys(secret, provisionKey) == 0);

  // So do raw references to the same bytes.
  assert(key_check::CheckLongTermKeys(crypto::KeyRef::Raw(access::vectors::M_SECRET, sizeof(access::vectors::M_SECRET)),
                                      crypto::KeyRef::Raw(access::vectors::M_PROVISION_KEY, sizeof(access::vectors::M_PROVISION_KEY))) == 0);

  // A missing provisioning key: -ENOKEY.
  crypto::host_kmu::Clear();
  crypto::host_kmu::Provision(kmu::M_SECRET_SLOT, access::vectors::M_SECRET, sizeof(access::vectors::M_SECRET));
  assert(key_check::CheckLongTermKeys(secret, provisionKey) == -ENOKEY);

  // A missing device secret: -ENOKEY.
  crypto::host_kmu::Clear();
  crypto::host_kmu::Provision(kmu::M_PROVISION_SLOT, access::vectors::M_PROVISION_KEY, sizeof(access::vectors::M_PROVISION_KEY));
  assert(key_check::CheckLongTermKeys(secret, provisionKey) == -ENOKEY);

  // An empty KMU: -ENOKEY.
  crypto::host_kmu::Clear();
  assert(key_check::CheckLongTermKeys(secret, provisionKey) == -ENOKEY);

  // A raw reference with no bytes cannot be used either: -ENOKEY.
  assert(key_check::CheckLongTermKeys(crypto::KeyRef::Raw(nullptr, 0),
                                      crypto::KeyRef::Raw(access::vectors::M_PROVISION_KEY, sizeof(access::vectors::M_PROVISION_KEY))) == -ENOKEY);

  // The same key in both slots: -EINVAL. A provisioner could then mint day keys.
  crypto::host_kmu::Clear();
  crypto::host_kmu::Provision(kmu::M_SECRET_SLOT, access::vectors::M_SECRET, sizeof(access::vectors::M_SECRET));
  crypto::host_kmu::Provision(kmu::M_PROVISION_SLOT, access::vectors::M_SECRET, sizeof(access::vectors::M_SECRET));
  assert(key_check::CheckLongTermKeys(secret, provisionKey) == -EINVAL);

  // An all-zero device secret: -EINVAL.
  crypto::host_kmu::Clear();
  crypto::host_kmu::Provision(kmu::M_SECRET_SLOT, M_ZERO_KEY, sizeof(M_ZERO_KEY));
  crypto::host_kmu::Provision(kmu::M_PROVISION_SLOT, access::vectors::M_PROVISION_KEY, sizeof(access::vectors::M_PROVISION_KEY));
  assert(key_check::CheckLongTermKeys(secret, provisionKey) == -EINVAL);

  // An all-zero provisioning key: -EINVAL.
  crypto::host_kmu::Clear();
  crypto::host_kmu::Provision(kmu::M_SECRET_SLOT, access::vectors::M_SECRET, sizeof(access::vectors::M_SECRET));
  crypto::host_kmu::Provision(kmu::M_PROVISION_SLOT, M_ZERO_KEY, sizeof(M_ZERO_KEY));
  assert(key_check::CheckLongTermKeys(secret, provisionKey) == -EINVAL);

  crypto::host_kmu::Clear();
  printf("key_check tests passed\n");
}
