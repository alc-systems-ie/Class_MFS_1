// HOST-ONLY crypto backend for the unit tests. Never linked into firmware.

#include <cerrno>
#include <memory>

#include <openssl/evp.h>
#include <openssl/hmac.h>

#include "crypto.hpp"

namespace alc::crypto
{

  int Init()
  {
    return 0;
  }

  int HmacSha256(const uint8_t* key, size_t keyLength, const uint8_t* message, size_t messageLength, uint8_t* out)
  {
    unsigned int outLength { 0 };

    if (HMAC(EVP_sha256(), key, static_cast<int>(keyLength), message, messageLength, out, &outLength) == nullptr) { return -EIO; }
    return outLength == M_HMAC_SHA256_BYTES ? 0 : -EIO;
  }

  namespace
  {
    using ContextPtr = std::unique_ptr<EVP_CIPHER_CTX, decltype(&EVP_CIPHER_CTX_free)>;

    int ccm(bool encrypt, const uint8_t* key, const uint8_t* nonce, size_t nonceLength, const uint8_t* aad, size_t aadLength, const uint8_t* input,
            size_t length, uint8_t* output, uint8_t* tag, size_t tagLength)
    {
      ContextPtr context { EVP_CIPHER_CTX_new(), &EVP_CIPHER_CTX_free };
      int outLength { 0 };
      int op { encrypt ? 1 : 0 };

      if (!context) { return -ENOMEM; }

      // CCM in OpenSSL is order-sensitive: nonce and tag lengths first, then key
      // and nonce, then the total data length, then AAD, then the data in ONE update.
      if (EVP_CipherInit_ex(context.get(), EVP_aes_128_ccm(), nullptr, nullptr, nullptr, op) != 1) { return -EIO; }
      if (EVP_CIPHER_CTX_ctrl(context.get(), EVP_CTRL_AEAD_SET_IVLEN, static_cast<int>(nonceLength), nullptr) != 1) { return -EIO; }
      if (EVP_CIPHER_CTX_ctrl(context.get(), EVP_CTRL_AEAD_SET_TAG, static_cast<int>(tagLength), encrypt ? nullptr : tag) != 1) { return -EIO; }
      if (EVP_CipherInit_ex(context.get(), nullptr, nullptr, key, nonce, op) != 1) { return -EIO; }
      if (EVP_CipherUpdate(context.get(), nullptr, &outLength, nullptr, static_cast<int>(length)) != 1) { return -EIO; }
      if (EVP_CipherUpdate(context.get(), nullptr, &outLength, aad, static_cast<int>(aadLength)) != 1) { return -EIO; }

      // For decrypt, this is where CCM reports a tag mismatch.
      if (EVP_CipherUpdate(context.get(), output, &outLength, input, static_cast<int>(length)) != 1) { return encrypt ? -EIO : -EBADMSG; }

      if (encrypt && EVP_CIPHER_CTX_ctrl(context.get(), EVP_CTRL_AEAD_GET_TAG, static_cast<int>(tagLength), tag) != 1) { return -EIO; }
      return 0;
    }
  }

  int AesCcmEncrypt(const uint8_t* key, const uint8_t* nonce, size_t nonceLength, const uint8_t* aad, size_t aadLength, const uint8_t* plaintext,
                    size_t length, uint8_t* ciphertext, uint8_t* tag, size_t tagLength)
  {
    return ccm(true, key, nonce, nonceLength, aad, aadLength, plaintext, length, ciphertext, tag, tagLength);
  }

  int AesCcmDecrypt(const uint8_t* key, const uint8_t* nonce, size_t nonceLength, const uint8_t* aad, size_t aadLength, const uint8_t* ciphertext,
                    size_t length, const uint8_t* tag, size_t tagLength, uint8_t* plaintext)
  {
    uint8_t tagCopy[16] {};

    if (tagLength > sizeof(tagCopy)) { return -EINVAL; }
    for (size_t index = 0; index < tagLength; index++) {
      tagCopy[index] = tag[index];
    }
    return ccm(false, key, nonce, nonceLength, aad, aadLength, ciphertext, length, plaintext, tagCopy, tagLength);
  }

}
