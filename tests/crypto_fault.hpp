#pragma once

// HOST-ONLY fault injection for the OpenSSL test backend (tests/crypto_openssl.cpp).
// Never in src/ and never in the firmware build.

namespace alc::crypto::host_fault
{

  /**
   * @brief Let callsBeforeFailure more HmacSha256() calls succeed, then fail every
   *        later call with -EIO until Clear().
   */
  void FailHmacAfter(int callsBeforeFailure);

  /** @brief Remove any armed fault. Call at the start and end of every test that arms one. */
  void Clear();

}
