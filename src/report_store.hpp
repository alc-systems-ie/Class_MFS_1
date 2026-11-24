#pragma once

#include <cstdint>

namespace alc::report_store
{

  /**
   * @brief Load the persisted report counter floor.
   *
   * Requires settings_subsys_init() to have run.
   *
   * @param generation The current report-key generation.
   * @param floor Set to the stored reservation, or 0 if no record exists or it was stored under an older generation.
   * @return 0 on success (including no record), -ESTALE if the record is from a newer generation than this image's
   *         (an older build flashed without an erase - its counter could reuse nonces), negative errno on other failure.
   */
  int Load(uint16_t generation, uint32_t& floor);

  /**
   * @brief ReportCounter::PersistFn. Returns only once the write has completed.
   *
   * `context` points at the current `uint16_t` generation. The record is generation (big-endian 16) then reservedUpTo (big-endian 32).
   */
  int Persist(uint32_t reservedUpTo, void* context);

}
