#include <cerrno>
#include <cstring>

#include "access_control.hpp"

namespace alc
{

  namespace
  {
    // The largest n whose full window cannot overflow. Unreachable in practice -
    // four billion commands in a day - but an overflow would wrap the window
    // back onto spent sequence numbers, which is a replay.
    constexpr uint32_t M_SEQUENCE_LIMIT { UINT32_MAX - AccessControl::M_WINDOW };

    struct Candidate
    {
        uint8_t slot;
        uint8_t offset;
        uint32_t n;
    };

    // A rotating-ID collision needs several candidates, but 128 in 2^32 per
    // advert means more than a handful is never going to happen.
    constexpr uint8_t M_MAX_CANDIDATES { 4 };
  }

  AccessControl::AccessControl(uint32_t deviceId, const uint8_t* secret, PersistFn persist, void* context)
      : m_device_id(deviceId)
      , m_secret(secret)
      , m_persist(persist)
      , m_persist_context(context)
      , m_state {}
      , m_tables_ready(false)
      , m_day_keys {}
      , m_expected_ids {}
      , m_window_size {}
      , m_failed_ids {}
      , m_id_failures {}
      , m_failures(0)
      , m_locked(false)
      , m_lockout_until_secs(0)
      , m_lockout_secs(M_LOCKOUT_INITIAL_SECS)
  {}

  void AccessControl::Restore(const AccessState& state)
  {
    m_state        = state;
    m_tables_ready = false;
  }

  void AccessControl::SetDeviceId(uint32_t deviceId)
  {
    m_device_id    = deviceId;
    m_tables_ready = false;
  }

  int AccessControl::rebuildSlot(uint8_t slot)
  {
    uint32_t base { m_state.next[slot] };
    uint8_t size { base > M_SEQUENCE_LIMIT ? static_cast<uint8_t>(0) : M_WINDOW };
    int result { 0 };

    for (uint8_t offset = 0; offset < size; offset++) {
      result = access::DeriveRotatingId(m_day_keys[slot], base + offset, m_expected_ids[slot][offset]);
      if (result < 0) { return result; }
    }
    m_window_size[slot] = size;
    m_failed_ids[slot]  = 0;
    memset(m_id_failures[slot], 0, sizeof(m_id_failures[slot]));
    return 0;
  }

  int AccessControl::prepareDay(uint16_t today, DeviceClock& clock)
  {
    AccessState previous { m_state };
    int result { 0 };

    if (m_tables_ready && today == m_state.day) { return 0; }

    // A new day: every slot starts again at zero, because the day keys are new.
    // Nothing about previous days is kept - their keys can no longer be derived
    // by the device, so their sequence numbers are dead weight.
    if (today > m_state.day) {
      m_state.day = today;
      memset(m_state.next, 0, sizeof(m_state.next));

      // Persist the rollover BEFORE building tables. If it cannot be saved the
      // day is not adopted, so no command can be accepted against unsaved state.
      result = m_persist(m_state, m_persist_context);
      if (result < 0) {
        m_state = previous;
        return result;
      }
    }

    // The floor follows the persisted day, never the other way round.
    clock.RaiseFloorDay(m_state.day);

    for (uint8_t slot = 0; slot < access::M_SLOT_COUNT; slot++) {
      result = access::DeriveDayKey(m_secret, m_device_id, m_state.day, slot, m_day_keys[slot]);
      if (result < 0) { return result; }
      result = rebuildSlot(slot);
      if (result < 0) { return result; }
    }

    m_tables_ready = true;
    return 0;
  }

  void AccessControl::recordFailure(int64_t uptimeSecs)
  {
    m_failures++;
    if (m_failures < M_LOCKOUT_THRESHOLD) { return; }

    // Doubling lockout, capped. Uptime rather than UTC, so a clock trim can
    // neither shorten nor extend it.
    m_locked             = true;
    m_lockout_until_secs = uptimeSecs + m_lockout_secs;
    m_lockout_secs       = m_lockout_secs * 2 > M_LOCKOUT_MAX_SECS ? M_LOCKOUT_MAX_SECS : m_lockout_secs * 2;
    m_failures           = 0;
  }

  AccessControl::Evaluation AccessControl::Evaluate(const uint8_t* onAir, uint8_t length, DeviceClock& clock, int64_t uptimeSecs)
  {
    Evaluation evaluation {};
    Candidate candidates[M_MAX_CANDIDATES] {};
    uint8_t candidateCount { 0 };
    uint8_t plaintext[protocol::M_PLAINTEXT_BYTES] {};
    uint16_t today { 0 };
    uint32_t previousNext { 0 };
    int result { 0 };
    bool authentic { false };

    if (onAir == nullptr || length < protocol::M_UUID_BYTES) { return evaluation; }

    // No trustworthy time, no day, no keys. See DeviceClock.
    if (!clock.IsValid()) {
      evaluation.verdict = Verdict::ClockInvalid;
      return evaluation;
    }

    today = clock.DayIndex(uptimeSecs);
    if (today < m_state.day) {
      // Cannot happen with a correct floor. Refuse rather than accept against a
      // day the device has already left.
      evaluation.verdict = Verdict::ClockInvalid;
      return evaluation;
    }

    result = prepareDay(today, clock);
    if (result < 0) {
      evaluation.verdict = Verdict::PersistFailed;
      return evaluation;
    }

    // Cheap filter first. Only a matching rotating ID costs a decryption, and
    // only a matching ID can count towards a lockout - an attacker without the
    // day key cannot predict one, so garbage cannot lock the engineer out.
    for (uint8_t slot = 0; slot < access::M_SLOT_COUNT; slot++) {
      for (uint8_t offset = 0; offset < m_window_size[slot]; offset++) {
        if (memcmp(m_expected_ids[slot][offset], &onAir[protocol::M_OFFSET_ROTATING_ID], protocol::M_ROTATING_ID_BYTES) != 0) { continue; }

        // A burned ID (M_MAX_ID_FAILURES wrong tags already) is treated exactly
        // like no match at all - not a candidate, not decrypted, not counted -
        // so an attacker who captured it cannot keep guessing its tag.
        if (m_id_failures[slot][offset] >= M_MAX_ID_FAILURES) { continue; }

        if (candidateCount < M_MAX_CANDIDATES) { candidates[candidateCount++] = Candidate { slot, offset, m_state.next[slot] + offset }; }
      }
    }

    if (candidateCount == 0) { return evaluation; }

    if (m_locked && uptimeSecs < m_lockout_until_secs) {
      evaluation.verdict = Verdict::LockedOut;
      return evaluation;
    }
    m_locked = false;

    for (uint8_t index = 0; index < candidateCount && !authentic; index++) {
      result = access::OpenCommand(m_day_keys[candidates[index].slot], m_device_id, m_state.day, candidates[index].slot, candidates[index].n, onAir,
                                   plaintext);
      if (result == 0) {
        authentic       = true;
        evaluation.slot = candidates[index].slot;
        evaluation.n    = candidates[index].n;
      } else if (result != -EBADMSG) {
        evaluation.verdict = Verdict::CryptoError;
        return evaluation;
      }
    }

    if (!authentic) {
      // Count each expected ID at most once towards the lockout. An attacker who
      // captures an unaccepted command can corrupt and resend it, but only that
      // ID (not every copy) counts, so a lockout requires 20 distinct IDs.
      bool newFailure { false };
      for (uint8_t index = 0; index < candidateCount; index++) {
        uint16_t bit { static_cast<uint16_t>(1U << candidates[index].offset) };
        if ((m_failed_ids[candidates[index].slot] & bit) == 0) {
          m_failed_ids[candidates[index].slot] |= bit;
          newFailure = true;
        }

        // The per-ID guess cap, unlike the bit above, keeps counting every
        // attempt against the SAME id - that is what lets it burn after
        // M_MAX_ID_FAILURES even though a resend no longer counts towards
        // the lockout.
        if (m_id_failures[candidates[index].slot][candidates[index].offset] < M_MAX_ID_FAILURES) {
          m_id_failures[candidates[index].slot][candidates[index].offset]++;
        }
      }
      if (newFailure) { recordFailure(uptimeSecs); }
      evaluation.verdict = Verdict::AuthFailed;
      return evaluation;
    }

    // Any authentic command clears the failure history.
    m_failures     = 0;
    m_lockout_secs = M_LOCKOUT_INITIAL_SECS;

    if (!protocol::DecodeCommand(plaintext, evaluation.command)) {
      evaluation.verdict = Verdict::Malformed;
      return evaluation;
    }

    // Freshness before consumption. A command captured, jammed and released
    // later fails here and stays unconsumed - which is harmless, because its
    // minute will never be fresh again and its key dies at 04:00.
    if (!clock.IsFresh(evaluation.command.minuteOfDay, uptimeSecs)) {
      evaluation.verdict = Verdict::Stale;
      return evaluation;
    }

    // PERSIST BEFORE ACTING. Reversed, a power loss between acting and saving
    // would leave the command replayable.
    previousNext                  = m_state.next[evaluation.slot];
    m_state.next[evaluation.slot] = evaluation.n + 1;
    result                        = m_persist(m_state, m_persist_context);
    if (result < 0) {
      m_state.next[evaluation.slot] = previousNext;
      evaluation.verdict            = Verdict::PersistFailed;
      return evaluation;
    }

    // A failed rebuild leaves stale IDs in the table, so force a full rebuild
    // on the next evaluation rather than trust it.
    if (rebuildSlot(evaluation.slot) < 0) { m_tables_ready = false; }
    evaluation.verdict = Verdict::Accepted;
    return evaluation;
  }

  int AccessControl::Advance(DeviceClock& clock, int64_t uptimeSecs)
  {
    uint16_t today { 0 };

    if (!clock.IsValid()) { return 0; }

    today = clock.DayIndex(uptimeSecs);
    if (today < m_state.day) { return -EINVAL; }

    return prepareDay(today, clock);
  }

}
