#pragma once

#include <cstdint>

namespace alc
{

  /**
   * @brief Event-report counter: the CCM nonce, never reused and never used before its reservation is persisted.
   *
   * Values are handed out from a reserved block. A block is written to non-volatile storage before any value in it
   * becomes usable, so after a reset the stored reservation is the new floor and every value below it is treated as spent.
   */
  class ReportCounter
  {
    public:
      /** Persists `reservedUpTo` (exclusive upper bound of the reservation). Returns 0 or a negative errno. */
      using PersistFn = int (*)(uint32_t reservedUpTo, void* context);

      static constexpr uint32_t M_BLOCK { 16 };
      static constexpr uint32_t M_REFILL_AT { 4 };

      ReportCounter(PersistFn persist, void* context);

      /**
     * @brief Start from the stored floor. Persists storedFloor + M_BLOCK first.
     * @return 0, the persist error (nothing usable), or -EOVERFLOW if the block would pass UINT32_MAX.
     */
      int Begin(uint32_t storedFloor);

      /** @brief Next usable value, or false if none is available. */
      bool Take(uint32_t& counter);

      /** @brief True when M_REFILL_AT or fewer reserved values remain unused. */
      bool NeedsRefill() const;

      /**
     * @brief Reserve another block. Persists first; on failure the remaining values stay usable and nothing beyond.
     * @return 0, the persist error, -EOVERFLOW rather than wrapping, or -EINVAL before Begin.
     */
      int Refill();

    private:
      PersistFn m_persist;
      void* m_context;
      uint32_t m_next { 0 };
      uint32_t m_usable_end { 0 };
      uint32_t m_reserved_up_to { 0 };
      bool m_begun { false };
  };

}
