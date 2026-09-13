// TARGET crypto backend: PSA Crypto, hardware-accelerated by CRACEN on the nRF54L05.
// Proven against tools/gen_access_vectors.py by crypto_selftest.cpp at boot.

#include <cerrno>

#include <psa/crypto.h>
#include <zephyr/logging/log.h>

#include "crypto.hpp"

LOG_MODULE_REGISTER(crypto_psa, LOG_LEVEL_INF);

namespace alc::crypto
{

  namespace
  {
    constexpr size_t M_HMAC_KEY_BITS { 256 };
    constexpr size_t M_AES128_KEY_BITS { 128 };
    constexpr size_t M_BITS_PER_BYTE { 8 };

    bool s_initialised { false };

    // Keys are imported, used once and destroyed. This phase holds raw key bytes
    // in RAM from Kconfig; production moves the device secret into the KMU and
    // derives there, at which point this seam is where that change lands.
    psa_status_t importKey(psa_key_type_t type, size_t bits, psa_key_usage_t usage, psa_algorithm_t algorithm, const uint8_t* key, psa_key_id_t& id)
    {
      psa_key_attributes_t attributes = PSA_KEY_ATTRIBUTES_INIT;

      psa_set_key_usage_flags(&attributes, usage);
      psa_set_key_lifetime(&attributes, PSA_KEY_LIFETIME_VOLATILE);
      psa_set_key_algorithm(&attributes, algorithm);
      psa_set_key_type(&attributes, type);
      psa_set_key_bits(&attributes, bits);
      return psa_import_key(&attributes, key, bits / M_BITS_PER_BYTE, &id);
    }

    psa_algorithm_t ccmAlgorithm(size_t tagLength)
    {
      return PSA_ALG_AEAD_WITH_SHORTENED_TAG(PSA_ALG_CCM, tagLength);
    }
  }

  int Init()
  {
    psa_status_t status { PSA_SUCCESS };

    if (s_initialised) { return 0; }

    status = psa_crypto_init();
    if (status != PSA_SUCCESS) {
      LOG_ERR("psa_crypto_init failed: %d!", status);
      return -EIO;
    }

    s_initialised = true;
    return 0;
  }

  int HmacSha256(const uint8_t* key, size_t keyLength, const uint8_t* message, size_t messageLength, uint8_t* out)
  {
    psa_key_id_t id { PSA_KEY_ID_NULL };
    size_t outLength { 0 };
    psa_status_t status { PSA_SUCCESS };

    // Every key in the scheme is exactly 256 bits. Anything else is a bug.
    if (keyLength * M_BITS_PER_BYTE != M_HMAC_KEY_BITS) { return -EINVAL; }

    status = importKey(PSA_KEY_TYPE_HMAC, M_HMAC_KEY_BITS, PSA_KEY_USAGE_SIGN_MESSAGE, PSA_ALG_HMAC(PSA_ALG_SHA_256), key, id);
    if (status != PSA_SUCCESS) {
      LOG_ERR("HMAC key import failed: %d!", status);
      return -EIO;
    }

    status = psa_mac_compute(id, PSA_ALG_HMAC(PSA_ALG_SHA_256), message, messageLength, out, M_HMAC_SHA256_BYTES, &outLength);
    psa_destroy_key(id);

    if (status != PSA_SUCCESS || outLength != M_HMAC_SHA256_BYTES) {
      LOG_ERR("psa_mac_compute failed: %d!", status);
      return -EIO;
    }
    return 0;
  }

  int AesCcmEncrypt(const uint8_t* key, const uint8_t* nonce, size_t nonceLength, const uint8_t* aad, size_t aadLength, const uint8_t* plaintext,
                    size_t length, uint8_t* ciphertext, uint8_t* tag, size_t tagLength)
  {
    constexpr size_t M_MAX_SEALED_BYTES { 64 };
    uint8_t sealed[M_MAX_SEALED_BYTES] {};
    psa_key_id_t id { PSA_KEY_ID_NULL };
    size_t outLength { 0 };
    psa_status_t status { PSA_SUCCESS };

    if (length + tagLength > sizeof(sealed)) { return -EINVAL; }

    status = importKey(PSA_KEY_TYPE_AES, M_AES128_KEY_BITS, PSA_KEY_USAGE_ENCRYPT, ccmAlgorithm(tagLength), key, id);
    if (status != PSA_SUCCESS) {
      LOG_ERR("AES key import failed: %d!", status);
      return -EIO;
    }

    // PSA emits ciphertext followed by the tag in one buffer.
    status = psa_aead_encrypt(id, ccmAlgorithm(tagLength), nonce, nonceLength, aad, aadLength, plaintext, length, sealed, sizeof(sealed), &outLength);
    psa_destroy_key(id);

    if (status != PSA_SUCCESS || outLength != length + tagLength) {
      LOG_ERR("psa_aead_encrypt failed: %d!", status);
      return -EIO;
    }

    for (size_t index = 0; index < length; index++) {
      ciphertext[index] = sealed[index];
    }
    for (size_t index = 0; index < tagLength; index++) {
      tag[index] = sealed[length + index];
    }
    return 0;
  }

  int AesCcmDecrypt(const uint8_t* key, const uint8_t* nonce, size_t nonceLength, const uint8_t* aad, size_t aadLength, const uint8_t* ciphertext,
                    size_t length, const uint8_t* tag, size_t tagLength, uint8_t* plaintext)
  {
    constexpr size_t M_MAX_SEALED_BYTES { 64 };
    uint8_t sealed[M_MAX_SEALED_BYTES] {};
    psa_key_id_t id { PSA_KEY_ID_NULL };
    size_t outLength { 0 };
    psa_status_t status { PSA_SUCCESS };

    if (length + tagLength > sizeof(sealed)) { return -EINVAL; }

    for (size_t index = 0; index < length; index++) {
      sealed[index] = ciphertext[index];
    }
    for (size_t index = 0; index < tagLength; index++) {
      sealed[length + index] = tag[index];
    }

    status = importKey(PSA_KEY_TYPE_AES, M_AES128_KEY_BITS, PSA_KEY_USAGE_DECRYPT, ccmAlgorithm(tagLength), key, id);
    if (status != PSA_SUCCESS) {
      LOG_ERR("AES key import failed: %d!", status);
      return -EIO;
    }

    status =
        psa_aead_decrypt(id, ccmAlgorithm(tagLength), nonce, nonceLength, aad, aadLength, sealed, length + tagLength, plaintext, length, &outLength);
    psa_destroy_key(id);

    // A tag mismatch is the ROUTINE outcome for a trial decryption, so it is
    // not logged here - AccessControl decides what it means.
    if (status == PSA_ERROR_INVALID_SIGNATURE) { return -EBADMSG; }
    if (status != PSA_SUCCESS || outLength != length) {
      LOG_ERR("psa_aead_decrypt failed: %d!", status);
      return -EIO;
    }
    return 0;
  }

}
