#include <cstring>

#include "report_keys.hpp"

namespace alc::report
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

    void buildAad(const uint8_t* reportId, uint8_t* aad)
    {
      memcpy(aad, reportId, M_REPORT_ID_BYTES);
      aad[M_REPORT_ID_BYTES] = M_REPORT_VERSION;
    }
  }

  int DeriveReportKey(const crypto::KeyRef& secret, uint32_t deviceId, uint16_t generation, uint8_t* reportKey)
  {
    constexpr uint8_t M_MESSAGE_BYTES { 7 };
    uint8_t message[M_MESSAGE_BYTES] {};

    putBe32(&message[0], deviceId);
    putBe16(&message[4], generation);
    message[6] = M_LABEL_REPORT_KEY;
    return crypto::HmacSha256(secret, message, sizeof(message), reportKey);
  }

  int DeriveReportEncKey(const uint8_t* reportKey, uint8_t* encKey)
  {
    const uint8_t message[] { M_LABEL_REPORT_ENC_KEY };
    uint8_t mac[crypto::M_HMAC_SHA256_BYTES] {};
    int result { crypto::HmacSha256(reportKey, M_REPORT_KEY_BYTES, message, sizeof(message), mac) };

    if (result < 0) { return result; }
    memcpy(encKey, mac, crypto::M_AES128_KEY_BYTES);
    return 0;
  }

  int DeriveReportId(const uint8_t* reportKey, uint32_t counter, uint8_t* reportId)
  {
    constexpr uint8_t M_MESSAGE_BYTES { 5 };
    uint8_t message[M_MESSAGE_BYTES] {};
    uint8_t mac[crypto::M_HMAC_SHA256_BYTES] {};
    int result { 0 };

    putBe32(&message[0], counter);
    message[4] = M_LABEL_REPORT_ID;
    result     = crypto::HmacSha256(reportKey, M_REPORT_KEY_BYTES, message, sizeof(message), mac);
    if (result < 0) { return result; }
    memcpy(reportId, mac, M_REPORT_ID_BYTES);
    return 0;
  }

  void BuildReportNonce(uint32_t deviceId, uint16_t generation, uint32_t counter, uint8_t* nonce)
  {
    putBe32(&nonce[0], deviceId);
    putBe16(&nonce[4], generation);
    nonce[6] = M_NONCE_MARKER;
    putBe32(&nonce[7], counter);
  }

  uint8_t BatteryTenths(int32_t millivolts)
  {
    constexpr int32_t M_ROUNDING_MV { 50 };
    constexpr int32_t M_MV_PER_TENTH { 100 };
    constexpr int32_t M_MAX_TENTHS { 254 };
    int32_t tenths { 0 };

    if (millivolts < 0) { return M_BATTERY_UNKNOWN; }
    tenths = (millivolts + M_ROUNDING_MV) / M_MV_PER_TENTH;
    return static_cast<uint8_t>(tenths > M_MAX_TENTHS ? M_MAX_TENTHS : tenths);
  }

  void EncodeReport(ReportEvent event, uint8_t batteryTenths, uint8_t* plaintext)
  {
    memset(plaintext, 0, M_PLAINTEXT_BYTES);
    plaintext[0] = static_cast<uint8_t>(event);
    plaintext[1] = batteryTenths;
  }

  int SealReport(const uint8_t* reportKey, uint32_t deviceId, uint16_t generation, uint32_t counter, const uint8_t* plaintext, uint8_t* onAir)
  {
    uint8_t encKey[crypto::M_AES128_KEY_BYTES] {};
    uint8_t nonce[M_NONCE_BYTES] {};
    uint8_t aad[M_AAD_BYTES] {};
    int result { DeriveReportId(reportKey, counter, onAir) };

    if (result < 0) { return result; }
    result = DeriveReportEncKey(reportKey, encKey);
    if (result < 0) { return result; }

    BuildReportNonce(deviceId, generation, counter, nonce);
    buildAad(onAir, aad);
    return crypto::AesCcmEncrypt(encKey, nonce, sizeof(nonce), aad, sizeof(aad), plaintext, M_PLAINTEXT_BYTES, &onAir[M_OFFSET_CIPHERTEXT],
                                 &onAir[M_OFFSET_TAG], M_TAG_BYTES);
  }

  int OpenReport(const uint8_t* reportKey, uint32_t deviceId, uint16_t generation, uint32_t counter, const uint8_t* onAir, uint8_t* plaintext)
  {
    uint8_t encKey[crypto::M_AES128_KEY_BYTES] {};
    uint8_t nonce[M_NONCE_BYTES] {};
    uint8_t aad[M_AAD_BYTES] {};
    int result { DeriveReportEncKey(reportKey, encKey) };

    if (result < 0) { return result; }

    BuildReportNonce(deviceId, generation, counter, nonce);
    buildAad(onAir, aad);
    return crypto::AesCcmDecrypt(encKey, nonce, sizeof(nonce), aad, sizeof(aad), &onAir[M_OFFSET_CIPHERTEXT], M_PLAINTEXT_BYTES, &onAir[M_OFFSET_TAG],
                                 M_TAG_BYTES, plaintext);
  }

}
