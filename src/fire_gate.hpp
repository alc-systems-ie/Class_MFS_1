#pragma once

#include <cstdint>

// Pure: no Zephyr headers, so the gate compiles and is tested on the host.
// Design: docs/superpowers/specs/2026-11-24-event-reports-design.md §5.2, §5.3.

namespace alc
{

  /**
   * @brief Holds the fire output back for the announce window after a fire condition begins.
   *
   * App calls Update() every tick with the raw fire condition at the single
   * output-derivation point. The first tick of a new condition starts the
   * announce window, raises one announce (TakeAnnounceDue()) so the fired
   * report can be sent, and returns false. The output may assert once
   * M_ANNOUNCE_MS has elapsed, or at once if Skip() was called (report
   * switched off, or it could not be built - fire anyway). A falling condition
   * resets everything, so a Disarm or a fault inside the window cancels the
   * fire and a later condition starts a fresh window.
   */
  class FireGate
  {
    public:
      static constexpr int64_t M_ANNOUNCE_MS { 100 };

      FireGate() = default;

      /** @brief Returns true when the output may assert for this tick. */
      bool Update(bool fireCondition, int64_t nowMs);

      /** @brief True once per new fire condition, on and after its first tick. */
      bool TakeAnnounceDue();

      /** @brief Permit the output at once. No effect unless a fire condition is being held. */
      void Skip();

    private:
      bool m_active { false };
      bool m_announce_due { false };
      bool m_skipped { false };
      int64_t m_start_ms { 0 };
  };

}
