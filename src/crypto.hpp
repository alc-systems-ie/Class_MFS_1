#pragma once

#include <cerrno>
#include <cstddef>
#include <cstdint>

// Not every libc defines it: neither the target's picolibc nor macOS does. The
// fallback must not collide with an errno the libc does define - 126, the Linux
// value, is ENETRESET in picolibc - so it sits above picolibc's __ELASTERROR
// (2000, "users can add values starting here") and macOS's ELAST (107).
#ifndef ENOKEY
#define ENOKEY 2001
#endif

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

  /**
   * @brief A key for HMAC: either raw bytes in RAM or a slot of the KMU key store.
   *        A KMU key is never read by firmware; the crypto engine uses it in place.
   */
  struct KeyRef
  {
      static KeyRef Raw(const uint8_t* bytes, size_t length) { return KeyRef { bytes, length, 0, false }; }
      static KeyRef Kmu(uint8_t slot) { return KeyRef { nullptr, 0, slot, true }; }
      bool IsKmu() const { return kmu; }

      const uint8_t* bytes;
      size_t length;
      uint8_t slot;
      bool kmu;
  };

  /** @brief HMAC-SHA256. @param out M_HMAC_SHA256_BYTES bytes. @return 0 or negative errno. */
  int HmacSha256(const uint8_t* key, size_t keyLength, const uint8_t* message, size_t messageLength, uint8_t* out);

  /**
   * @brief HMAC-SHA256 under a raw or KMU key.
   * @return 0; -ENOKEY if a KMU slot is not provisioned; other negative errno on a backend fault. `out` is untouched on failure.
   */
  int HmacSha256(const KeyRef& key, const uint8_t* message, size_t messageLength, uint8_t* out);

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
