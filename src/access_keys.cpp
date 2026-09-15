#include <cerrno>
#include <cstring>

#include "access_keys.hpp"

namespace alc::access
{

  namespace
  {
    void putBe32(uint8_t* out, uint32_t value)
    {
      out[0] = static_cast<uint8_t>(value >> 24);
      out[1] = static_cast<uint8_t>(value >> 16);
      out[2] = static_cast<uint8_t>(value >> 8);
      out[3] = static_cast<uint8_t>(value);
    }

    void putBe16(uint8_t* out, uint16_t value)
    {
      out[0] = static_cast<uint8_t>(value >> 8);
      out[1] = static_cast<uint8_t>(value);
    }

    void buildAad(const uint8_t* onAir, uint8_t* aad)
    {
      // The version is NEVER on air. Both sides supply it, so a payload from
      // another protocol version fails authentication instead of mis-parsing.
      memcpy(aad, &onAir[protocol::M_OFFSET_ROTATING_ID], protocol::M_ROTATING_ID_BYTES);
      aad[protocol::M_ROTATING_ID_BYTES] = protocol::M_PROTOCOL_VERSION;
    }
  }

  int DeriveDayKey(const uint8_t* secret, uint32_t deviceId, uint16_t day, uint8_t slot, uint8_t* dayKey)
  {
    constexpr uint8_t M_MESSAGE_BYTES { 8 };
    uint8_t message[M_MESSAGE_BYTES] {};

    putBe32(&message[0], deviceId);
    putBe16(&message[4], day);
    message[6] = slot;
    message[7] = M_LABEL_DAY_KEY;
    return crypto::HmacSha256(secret, M_SECRET_BYTES, message, sizeof(message), dayKey);
  }

  int DeriveEncKey(const uint8_t* dayKey, uint8_t* encKey)
  {
    const uint8_t message[] { M_LABEL_ENC_KEY };
    uint8_t mac[crypto::M_HMAC_SHA256_BYTES] {};
    int result { crypto::HmacSha256(dayKey, M_DAY_KEY_BYTES, message, sizeof(message), mac) };

    if (result < 0) { return result; }
    memcpy(encKey, mac, crypto::M_AES128_KEY_BYTES);
    return 0;
  }

  int DeriveRotatingId(const uint8_t* dayKey, uint32_t n, uint8_t* rotatingId)
  {
    constexpr uint8_t M_MESSAGE_BYTES { 5 };
    uint8_t message[M_MESSAGE_BYTES] {};
    uint8_t mac[crypto::M_HMAC_SHA256_BYTES] {};
    int result { 0 };

    putBe32(&message[0], n);
    message[4] = M_LABEL_ROTATING_ID;
    result     = crypto::HmacSha256(dayKey, M_DAY_KEY_BYTES, message, sizeof(message), mac);
    if (result < 0) { return result; }
    memcpy(rotatingId, mac, protocol::M_ROTATING_ID_BYTES);
    return 0;
  }

  int ConfirmId(const uint8_t* dayKey, uint32_t n, uint8_t event, uint8_t* confirmId)
  {
    constexpr uint8_t M_MESSAGE_BYTES { 6 };
    uint8_t message[M_MESSAGE_BYTES] {};
    uint8_t mac[crypto::M_HMAC_SHA256_BYTES] {};
    int result { 0 };

    putBe32(&message[0], n);
    message[4] = event;
    message[5] = M_LABEL_CONFIRM;
    result     = crypto::HmacSha256(dayKey, M_DAY_KEY_BYTES, message, sizeof(message), mac);
    if (result < 0) { return result; }
    memcpy(confirmId, mac, protocol::M_UUID_BYTES);
    return 0;
  }

  void BuildNonce(uint32_t deviceId, uint16_t day, uint8_t slot, uint32_t n, uint8_t* nonce)
  {
    putBe32(&nonce[0], deviceId);
    putBe16(&nonce[4], day);
    nonce[6] = slot;
    putBe32(&nonce[7], n);
  }

  int SealCommand(const uint8_t* dayKey, uint32_t deviceId, uint16_t day, uint8_t slot, uint32_t n, const uint8_t* plaintext, uint8_t* onAir)
  {
    uint8_t encKey[crypto::M_AES128_KEY_BYTES] {};
    uint8_t nonce[M_NONCE_BYTES] {};
    uint8_t aad[M_AAD_BYTES] {};
    int result { DeriveRotatingId(dayKey, n, &onAir[protocol::M_OFFSET_ROTATING_ID]) };

    if (result < 0) { return result; }
    result = DeriveEncKey(dayKey, encKey);
    if (result < 0) { return result; }

    BuildNonce(deviceId, day, slot, n, nonce);
    buildAad(onAir, aad);
    return crypto::AesCcmEncrypt(encKey, nonce, sizeof(nonce), aad, sizeof(aad), plaintext, protocol::M_PLAINTEXT_BYTES,
                                 &onAir[protocol::M_OFFSET_CIPHERTEXT], &onAir[protocol::M_OFFSET_TAG], protocol::M_TAG_BYTES);
  }

  int OpenCommand(const uint8_t* dayKey, uint32_t deviceId, uint16_t day, uint8_t slot, uint32_t n, const uint8_t* onAir, uint8_t* plaintext)
  {
    uint8_t encKey[crypto::M_AES128_KEY_BYTES] {};
    uint8_t nonce[M_NONCE_BYTES] {};
    uint8_t aad[M_AAD_BYTES] {};
    int result { DeriveEncKey(dayKey, encKey) };

    if (result < 0) { return result; }

    BuildNonce(deviceId, day, slot, n, nonce);
    buildAad(onAir, aad);
    return crypto::AesCcmDecrypt(encKey, nonce, sizeof(nonce), aad, sizeof(aad), &onAir[protocol::M_OFFSET_CIPHERTEXT], protocol::M_PLAINTEXT_BYTES,
                                 &onAir[protocol::M_OFFSET_TAG], protocol::M_TAG_BYTES, plaintext);
  }

  namespace
  {
    int timeSyncTag(const uint8_t* provisionKey, uint32_t deviceId, uint32_t unixSeconds, uint8_t* tag)
    {
      constexpr uint8_t M_MESSAGE_BYTES { 9 };
      uint8_t message[M_MESSAGE_BYTES] {};
      uint8_t mac[crypto::M_HMAC_SHA256_BYTES] {};
      int result { 0 };

      putBe32(&message[0], deviceId);
      putBe32(&message[4], unixSeconds);
      message[8] = M_LABEL_TIME_SYNC;
      result     = crypto::HmacSha256(provisionKey, M_SECRET_BYTES, message, sizeof(message), mac);
      if (result < 0) { return result; }
      memcpy(tag, mac, M_TIME_SYNC_TAG_BYTES);
      return 0;
    }
  }

  int BuildTimeSync(const uint8_t* provisionKey, uint32_t deviceId, uint32_t unixSeconds, uint8_t* onAir)
  {
    onAir[0] = static_cast<uint8_t>(unixSeconds);
    onAir[1] = static_cast<uint8_t>(unixSeconds >> 8);
    onAir[2] = static_cast<uint8_t>(unixSeconds >> 16);
    onAir[3] = static_cast<uint8_t>(unixSeconds >> 24);
    return timeSyncTag(provisionKey, deviceId, unixSeconds, &onAir[4]);
  }

  bool OpenTimeSync(const uint8_t* provisionKey, uint32_t deviceId, const uint8_t* onAir, uint32_t& unixSeconds)
  {
    uint8_t expected[M_TIME_SYNC_TAG_BYTES] {};
    uint32_t presented { static_cast<uint32_t>(onAir[0]) | (static_cast<uint32_t>(onAir[1]) << 8) | (static_cast<uint32_t>(onAir[2]) << 16) |
                         (static_cast<uint32_t>(onAir[3]) << 24) };

    if (timeSyncTag(provisionKey, deviceId, presented, expected) < 0) { return false; }
    if (!crypto::ConstantTimeEqual(expected, &onAir[4], M_TIME_SYNC_TAG_BYTES)) { return false; }

    unixSeconds = presented;
    return true;
  }

}
