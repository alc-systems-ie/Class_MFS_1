#pragma once

#include <cstddef>
#include <cstdint>

namespace alc::crypto
{

  // The one seam between the access scheme and a crypto library. On target it
  // is backed by PSA (CRACEN, src/crypto_psa.cpp); on the host by OpenSSL
  // (tests/crypto_openssl.cpp). Both are proven against the same Python-generated
  // vectors, so neither is trusted on its own word.

  constexpr size_t M_HMAC_SHA256_BYTES { 32 };
  constexpr size_t M_AES128_KEY_BYTES { 16 };

  /** @brief Initialise the backend. Idempotent. @return 0 or negative errno. */
  int Init();

  /** @brief HMAC-SHA256. @param out M_HMAC_SHA256_BYTES bytes. @return 0 or negative errno. */
  int HmacSha256(const uint8_t* key, size_t keyLength, const uint8_t* message, size_t messageLength, uint8_t* out);

  /**
   * @brief AES-128-CCM encrypt with a shortened tag.
   * @return 0 or negative errno.
   */
  int AesCcmEncrypt(const uint8_t* key, const uint8_t* nonce, size_t nonceLength, const uint8_t* aad, size_t aadLength, const uint8_t* plaintext,
                    size_t length, uint8_t* ciphertext, uint8_t* tag, size_t tagLength);

  /**
   * @brief AES-128-CCM decrypt and verify.
   * @return 0 if authentic; -EBADMSG if the tag does not verify; other negative errno on a backend fault.
   *         On any failure `plaintext` must not be used.
   */
  int AesCcmDecrypt(const uint8_t* key, const uint8_t* nonce, size_t nonceLength, const uint8_t* aad, size_t aadLength, const uint8_t* ciphertext,
                    size_t length, const uint8_t* tag, size_t tagLength, uint8_t* plaintext);

  /** @brief Compare without an early exit, so timing does not reveal how many bytes matched. */
  inline bool ConstantTimeEqual(const uint8_t* a, const uint8_t* b, size_t length)
  {
    uint8_t difference { 0 };

    for (size_t index = 0; index < length; index++) {
      difference = static_cast<uint8_t>(difference | (a[index] ^ b[index]));
    }
    return difference == 0;
  }

}
