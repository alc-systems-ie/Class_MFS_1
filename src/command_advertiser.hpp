#pragma once

#include <cstdint>

namespace alc
{

  /**
   * @brief Brief non-connectable BLE advertising burst for an event report.
   *
   * MFS_1 is otherwise observer-only (`CommandScanner`, command_scanner.hpp) —
   * see docs/tan-scheme.md section 6.1: the device never advertises to solicit
   * contact. An event report (docs/superpowers/specs/2026-11-24-event-reports-design.md
   * section 5) is the documented exception: a 100 ms burst sent only while the
   * device is Active (or as it leaves Active), so advertising is off the power
   * path otherwise.
   *
   * Advertises exactly one 128-bit service UUID — the 16 sealed report bytes
   * (`report::SealReport()`, report_keys.hpp) — as a Complete List of 128-bit
   * Service UUIDs (AD type 0x07), in array order, unreversed
   * (`BT_DATA(BT_DATA_UUID128_ALL, uuid, 16)`, command_advertiser.cpp), the same
   * AD type and array order `CommandScanner` scans for (see its
   * `M_AD_UUID128_ALL`).
   *
   * Bluetooth is already enabled by `CommandScanner::Start()` at boot; this
   * class MUST NOT call `bt_enable()` — it only calls `bt_le_adv_start()` /
   * `bt_le_adv_stop()`. Non-connectable and non-scannable, with no identity
   * (a non-resolvable private address), at the fastest legal interval (20 ms),
   * matching the covert posture of the rest of the device — no name, no GATT,
   * no connections. Zephyr's `samples/bluetooth/scan_adv` demonstrates the
   * Observer and Broadcaster roles coexisting on one controller, which is
   * exactly this class running alongside the existing `CommandScanner`. A
   * 100 ms burst may perturb the scan; the scanner-loss fail-safe still applies
   * (it is keyed on the scanner stopping, not on missed windows).
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
       * @param txPowerDbm Requested power at the antenna, dBm (`ReportPolicy::TxPowerDbm()`). Each burst sets its own power, before and
       *                   after the advert starts. A failure to set it is logged and the
       *                   burst goes out at the controller's current power; it never changes the result.
       * @return 0 if the advert started; the bt_le_adv_start() error otherwise.
       */
      int Burst(const uint8_t uuid[16], int64_t nowMs, int64_t durationMs, int8_t txPowerDbm);

      /**
       * @brief Stop the burst once its duration has elapsed. No-op otherwise.
       *
       * Call periodically (main loop cadence) while a burst may be running.
       */
      void Service(int64_t nowMs);

      /** @brief True from a successful Burst() until Service() stops it. */
      bool IsAdvertising() const { return m_advertising; }

    private:
      /** Sends the vendor Write TX Power command for the advertiser (legacy advertising ignores the handle). Logs only. */
      void setTxPower(int8_t txPowerDbm);

      bool m_advertising;
      int64_t m_stop_deadline_ms;
  };

}
