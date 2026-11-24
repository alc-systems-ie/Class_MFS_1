#pragma once

#include <cstdint>

#include "crypto.hpp"

namespace alc::report
{

  // The event-report derivations of docs/superpowers/specs/2026-11-24-event-reports-design.md
  // section 4. Every function here is mirrored byte for byte by the Network Manager and
  // checked against tools/gen_access_vectors.py. Labels 0x06 (confirmId, retired) and 0x07
  // (boot key check) are never used here.

  constexpr uint8_t M_LABEL_REPORT_KEY { 0x08 };
  constexpr uint8_t M_LABEL_REPORT_ENC_KEY { 0x09 };
  constexpr uint8_t M_LABEL_REPORT_ID { 0x0A };

  // Authenticated, never transmitted: both sides supply it as CCM associated data.
  constexpr uint8_t M_REPORT_VERSION { 0x01 };

  // Sits where a command nonce carries its slot, so a report nonce can never equal a command nonce.
  constexpr uint8_t M_NONCE_MARKER { 0xFF };

  constexpr uint8_t M_REPORT_KEY_BYTES { 32 };
  constexpr uint8_t M_NONCE_BYTES { 11 };
  constexpr uint8_t M_AAD_BYTES { 5 };
  constexpr uint8_t M_PLAINTEXT_BYTES { 8 };

  // On air: reportId(4) | ciphertext(8) | tag(4).
  constexpr uint8_t M_REPORT_ID_BYTES { 4 };
  constexpr uint8_t M_OFFSET_CIPHERTEXT { 4 };
  constexpr uint8_t M_OFFSET_TAG { 12 };
  constexpr uint8_t M_TAG_BYTES { 4 };
  constexpr uint8_t M_ON_AIR_BYTES { 16 };

  // Battery byte: tenths of a volt, rounded. 0xFF means not measured.
  constexpr uint8_t M_BATTERY_UNKNOWN { 0xFF };

  enum class ReportEvent : uint8_t {
    LowBattery       = 0x01,
    Activated        = 0x02,
    Deactivated      = 0x03,
    FiredFromCommand = 0x10,
    FiredFromMotion  = 0x11,
    FiredFromSound   = 0x12, // Never sent by MFS_1.
  };

  /**
   * @brief reportKey = HMAC-SHA256(secret, id BE32 | generation BE16 | 0x08).
   * @param secret The device secret: a KMU slot on target, raw bytes in tests.
   * @return 0; -ENOKEY if a KMU secret is not provisioned; other negative errno on a backend fault.
   */
  int DeriveReportKey(const crypto::KeyRef& secret, uint32_t deviceId, uint16_t generation, uint8_t* reportKey);

  /** @brief encKey = first 16 bytes of HMAC-SHA256(reportKey, 0x09). */
  int DeriveReportEncKey(const uint8_t* reportKey, uint8_t* encKey);

  /** @brief reportId(r) = first 4 bytes of HMAC-SHA256(reportKey, r BE32 | 0x0A). */
  int DeriveReportId(const uint8_t* reportKey, uint32_t counter, uint8_t* reportId);

  /** @brief nonce = id BE32 | generation BE16 | 0xFF | r BE32 (11 bytes). */
  void BuildReportNonce(uint32_t deviceId, uint16_t generation, uint32_t counter, uint8_t* nonce);

  /** @brief Battery tenths of a volt, rounded: (mv + 50) / 100, clamped to 254. Negative means not measured (0xFF). */
  uint8_t BatteryTenths(int32_t millivolts);

  /** @brief Plaintext: byte 0 event, byte 1 battery tenths, bytes 2-7 zero. */
  void EncodeReport(ReportEvent event, uint8_t batteryTenths, uint8_t* plaintext);

  /** @brief Build the 16 on-air bytes for report number `counter`. AAD is reportId | M_REPORT_VERSION. */
  int SealReport(const uint8_t* reportKey, uint32_t deviceId, uint16_t generation, uint32_t counter, const uint8_t* plaintext, uint8_t* onAir);

  /**
   * @brief Authenticate and decrypt 16 on-air bytes as report number `counter`. Used by the tests; the Network Manager does this in service.
   * @return 0 if authentic; -EBADMSG if not; other negative errno on a backend fault.
   */
  int OpenReport(const uint8_t* reportKey, uint32_t deviceId, uint16_t generation, uint32_t counter, const uint8_t* onAir, uint8_t* plaintext);

}
