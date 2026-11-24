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
   * @param floor Set to the stored reservation, or 0 if no record exists or it was stored under a different generation.
   * @return 0 on success (including no record), negative errno on failure.
   */
  int Load(uint16_t generation, uint32_t& floor);

  /**
   * @brief ReportCounter::PersistFn. Returns only once the write has completed.
   *
   * `context` points at the current `uint16_t` generation. The record is generation (big-endian 16) then reservedUpTo (big-endian 32).
   */
  int Persist(uint32_t reservedUpTo, void* context);

}
