#pragma once

#include "access_control.hpp"

namespace alc::access_store
{

  /**
   * @brief Load the persisted access state.
   *
   * Requires settings_subsys_init() to have run.
   *
   * @param state Written only if a valid record exists.
   * @return 1 if a record was loaded, 0 if none exists (a first boot), negative errno on failure,
   *         including a record that exists but is invalid.
   */
  int Load(AccessState& state);

  /**
   * @brief AccessControl::PersistFn. Returns only once the write has completed.
   *
   * `context` is unused.
   */
  int Persist(const AccessState& state, void* context);

}
