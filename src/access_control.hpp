#pragma once

#include <cstdint>

#include "access_keys.hpp"
#include "device_clock.hpp"
#include "mfs_protocol.hpp"

namespace alc
{

  /** @brief The persisted access state. 36 bytes. Written only on change. */
  struct AccessState
  {
      uint16_t day { 0 };
      uint32_t next[access::M_SLOT_COUNT] {};
  };

  /**
   * @brief Decides whether 16 on-air bytes are an authentic, fresh command.
   *
   * Pure logic apart from the persistence hook, so the whole rule set is
   * host-tested. See docs/tan-scheme.md section 6.
   *
   * **Silence is the default.** Every verdict other than Accepted results in no
   * radio emission and no LED - the caller logs over RTT and does nothing else.
   */
  class AccessControl
  {
    public:
      // Look-ahead per slot. The phone never learns whether a command landed, so
      // the device accepts any of the next 16 sequence numbers and skips past it.
      static constexpr uint8_t M_WINDOW { 16 };

      // Consecutive authentication failures that trigger a lockout.
      static constexpr uint8_t M_LOCKOUT_THRESHOLD { 20 };
      static constexpr uint32_t M_LOCKOUT_INITIAL_SECS { 600 };
      static constexpr uint32_t M_LOCKOUT_MAX_SECS { 14400 };

      enum class Verdict : uint8_t {
        Accepted,
        NotForUs,      ///< No expected rotating ID matched. The normal case for any other advert. Not counted.
        ClockInvalid,  ///< No trustworthy time yet - only a provisioner sync is listened for.
        LockedOut,     ///< An ID matched during a lockout. Not decrypted, not counted.
        AuthFailed,    ///< An ID matched but no candidate authenticated. Counted.
        Malformed,     ///< Authentic, but the plaintext is invalid. Not consumed.
        Stale,         ///< Authentic, but the minute is outside the freshness window. Not consumed.
        PersistFailed, ///< Authentic and fresh, but the sequence number could not be saved. NOT acted on.
        CryptoError,   ///< Backend fault. Not counted.
      };

      struct Evaluation
      {
          Verdict verdict { Verdict::NotForUs };
          uint8_t slot { 0 };
          uint32_t n { 0 };
          protocol::Command command {};
      };

      /** @brief Persistence hook. Must return only once the state is durable. */
      using PersistFn = int (*)(const AccessState& state, void* context);

      /**
       * @param deviceId Public 32-bit identity used in every derivation.
       * @param secret   M_SECRET_BYTES. Must outlive this object.
       * @param persist  Called before any command is acted on.
       */
      AccessControl(uint32_t deviceId, const uint8_t* secret, PersistFn persist, void* context);

      /** @brief Adopt a state loaded from NVS. Call before the first Evaluate(). */
      void Restore(const AccessState& state);

      const AccessState& State() const { return m_state; }
      bool IsLockedOut(int64_t uptimeSecs) const { return m_locked && uptimeSecs < m_lockout_until_secs; }
      uint8_t ConsecutiveFailures() const { return m_failures; }

      /**
       * @brief Evaluate one 128-bit service UUID's bytes.
       *
       * On Accepted the slot's sequence number has ALREADY been persisted past n,
       * so the command cannot be replayed however the caller then fails.
       */
      Evaluation Evaluate(const uint8_t* onAir, uint8_t length, DeviceClock& clock, int64_t uptimeSecs);

    private:
      int prepareDay(uint16_t today, DeviceClock& clock);
      int rebuildSlot(uint8_t slot);
      void recordFailure(int64_t uptimeSecs);

      uint32_t m_device_id;
      const uint8_t* m_secret;
      PersistFn m_persist;
      void* m_persist_context;

      AccessState m_state;
      bool m_tables_ready;
      uint8_t m_day_keys[access::M_SLOT_COUNT][access::M_DAY_KEY_BYTES];
      uint8_t m_expected_ids[access::M_SLOT_COUNT][M_WINDOW][protocol::M_ROTATING_ID_BYTES];
      uint8_t m_window_size[access::M_SLOT_COUNT];

      uint8_t m_failures;
      bool m_locked;
      int64_t m_lockout_until_secs;
      uint32_t m_lockout_secs;
  };

}
