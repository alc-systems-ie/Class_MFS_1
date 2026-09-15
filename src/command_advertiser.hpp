#pragma once

#include <cstdint>

namespace alc
{

  /**
   * @brief Brief non-connectable BLE advertising burst for the FIRE confirmation.
   *
   * MFS_1 is otherwise observer-only (`CommandScanner`, command_scanner.hpp) —
   * see docs/tan-scheme.md section 6.1: the device never advertises to solicit
   * contact. A FIRE confirmation is a documented exception to that rule
   * (`protocol::Mode::ReportAndTrigger` / `ReportOnly`, mfs_protocol.hpp), so
   * advertising happens ONLY on FIRE and is off the power path otherwise.
   *
   * Advertises exactly one 128-bit service UUID — the confirmId
   * (`AccessKeys::ConfirmId()`, access_keys.hpp) — as a Complete List of
   * 128-bit Service UUIDs (AD type 0x07), the same AD type and on-air byte
   * order `CommandScanner` scans for (see its `M_AD_UUID128_ALL`), so the
   * app's own scan (Task 12) matches it with a plain memcmp — no byte
   * reversal either side of the radio.
   *
   * Bluetooth is already enabled by `CommandScanner::Start()` at boot; this
   * class MUST NOT call `bt_enable()` — it only calls `bt_le_adv_start()` /
   * `bt_le_adv_stop()`. Non-connectable and non-scannable, with no identity
   * (a non-resolvable private address), matching the covert posture of the
   * rest of the device — no name, no GATT, no connections. Zephyr's
   * `samples/bluetooth/scan_adv` demonstrates the Observer and Broadcaster
   * roles coexisting on one controller, which is exactly this class running
   * alongside the existing `CommandScanner`. A brief adv burst is acceptable
   * even if it perturbs the scan — the fire countdown is scanner-loss-exempt
   * (`FireSequence`, fire_sequence.hpp).
   */
  class CommandAdvertiser
  {
    public:
      CommandAdvertiser();

      /**
       * @brief Start a non-connectable advertising burst of `uuid`.
       *
       * Restarts cleanly if a burst is already running: the previous advert is
       * stopped before the new one starts, so two Burst() calls in quick
       * succession never leave two adverts contending for the radio.
       *
       * @param uuid On-air byte order, 16 bytes — the same bytes CommandScanner
       *             would recognise, unreversed.
       * @param nowMs Current uptime, ms (`k_uptime_get()`).
       * @param durationMs How long to advertise before Service() stops it.
       */
      void Burst(const uint8_t uuid[16], int64_t nowMs, int64_t durationMs);

      /**
       * @brief Stop the burst once its duration has elapsed. No-op otherwise.
       *
       * Call periodically (main loop cadence) while a burst may be running.
       */
      void Service(int64_t nowMs);

      /** @brief True from a successful Burst() until Service() stops it. */
      bool IsAdvertising() const { return m_advertising; }

    private:
      bool m_advertising;
      int64_t m_stop_deadline_ms;
  };

}
