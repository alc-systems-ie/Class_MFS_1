#pragma once

#include <cstdint>

#include "crypto.hpp"
#include "mfs_protocol.hpp"

namespace alc::access
{

  // The derivations of docs/tan-scheme.md section 3. Every function here is
  // mirrored byte for byte by class_app/lib/protocol/access_keys.dart, and both
  // are checked against tools/gen_access_vectors.py.

  constexpr uint8_t M_SECRET_BYTES { 32 };
  constexpr uint8_t M_DAY_KEY_BYTES { 32 };
  constexpr uint8_t M_NONCE_BYTES { 11 };
  constexpr uint8_t M_AAD_BYTES { 5 };

  // Eight key slots per device per day. Slot 0 is the Network Manager's own and
  // is the only slot whose commands may change the operating mode. Slots 1-7 are
  // assigned to engineers by the Network Manager, per device and per day.
  constexpr uint8_t M_SLOT_COUNT { 8 };
  constexpr uint8_t M_SLOT_NETWORK_MANAGER { 0 };

  // Domain-separation labels. 0x00 and 0x01 were the retired paper-TAN and
  // session-key labels and must not be reused.
  constexpr uint8_t M_LABEL_DAY_KEY { 0x02 };
  constexpr uint8_t M_LABEL_ENC_KEY { 0x03 };
  constexpr uint8_t M_LABEL_ROTATING_ID { 0x04 };
  constexpr uint8_t M_LABEL_TIME_SYNC { 0x05 };
  constexpr uint8_t M_LABEL_CONFIRM { 0x06 };

  constexpr uint8_t M_TIME_SYNC_TAG_BYTES { 12 };

  /** @brief dayKey = HMAC-SHA256(secret, id BE32 | day BE16 | slot | 0x02). */
  int DeriveDayKey(const uint8_t* secret, uint32_t deviceId, uint16_t day, uint8_t slot, uint8_t* dayKey);

  /** @brief encKey = first 16 bytes of HMAC-SHA256(dayKey, 0x03). */
  int DeriveEncKey(const uint8_t* dayKey, uint8_t* encKey);

  /** @brief rotatingId(n) = first 4 bytes of HMAC-SHA256(dayKey, n BE32 | 0x04). */
  int DeriveRotatingId(const uint8_t* dayKey, uint32_t n, uint8_t* rotatingId);

  /** @brief confirmId(n, event) = first 16 bytes of HMAC-SHA256(dayKey, n BE32 | event | 0x06). */
  int ConfirmId(const uint8_t* dayKey, uint32_t n, uint8_t event, uint8_t* confirmId);

  /** @brief nonce = id BE32 | day BE16 | slot | n BE32 (11 bytes). */
  void BuildNonce(uint32_t deviceId, uint16_t day, uint8_t slot, uint32_t n, uint8_t* nonce);

  /**
   * @brief Build the 16 on-air bytes for command number n.
   *
   * Used by the host tests and the on-target self-test. The device itself only
   * ever opens commands; the app seals them.
   */
  int SealCommand(const uint8_t* dayKey, uint32_t deviceId, uint16_t day, uint8_t slot, uint32_t n, const uint8_t* plaintext, uint8_t* onAir);

  /**
   * @brief Authenticate and decrypt 16 on-air bytes as command number n.
   * @return 0 if authentic; -EBADMSG if not; other negative errno on a backend fault.
   */
  int OpenCommand(const uint8_t* dayKey, uint32_t deviceId, uint16_t day, uint8_t slot, uint32_t n, const uint8_t* onAir, uint8_t* plaintext);

  /** @brief Time sync on air: unix LE32 | first 12 bytes of HMAC-SHA256(provisionKey, id BE32 | unix BE32 | 0x05). */
  int BuildTimeSync(const uint8_t* provisionKey, uint32_t deviceId, uint32_t unixSeconds, uint8_t* onAir);

  /**
   * @brief Verify a time sync. Says nothing about whether the time is acceptable -
   *        that is DeviceClock::ApplyProvisionerSync().
   * @return True only if the tag verifies; `unixSeconds` is written only then.
   */
  bool OpenTimeSync(const uint8_t* provisionKey, uint32_t deviceId, const uint8_t* onAir, uint32_t& unixSeconds);

}
