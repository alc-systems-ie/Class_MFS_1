#pragma once

#include <cstdint>

#include "mfs_protocol.hpp"

namespace alc
{

  /**
   * @brief BLE observer that hands candidate command bytes to the main loop.
   *
   * MFS_1 never advertises and never connects. The engineer's phone advertises a
   * 128-bit service UUID; this scans passively for 16-byte UUIDs and queues them.
   *
   * **It validates nothing.** It cannot: a command is only recognisable with the
   * day keys, and a time sync only with the provisioning key. Every decision is
   * made by the main loop through AccessControl and DeviceClock, so no security
   * logic runs on the Bluetooth RX thread.
   *
   * It suppresses repeats of the UUIDs it queued most recently. A phone advertises
   * each command ~160 times in 30 s; without this the queue would fill with copies
   * and a distinct command arriving alongside them could be dropped.
   */
  class CommandScanner
  {
    public:
      /** @brief One queued 128-bit service UUID, on-air byte order. */
      struct Candidate
      {
          uint8_t bytes[protocol::M_UUID_BYTES];
      };

      CommandScanner();

      /**
       * @brief Enable Bluetooth and start the passive scan.
       * @return 0 on success; negative errno from bt_enable() or bt_le_scan_start().
       */
      int Start();

      /**
       * @brief Take the oldest queued candidate, if any. Never blocks.
       * @return True if `out` was written.
       */
      bool TakeCandidate(Candidate& out);

      /**
       * @brief Number of adverts dropped for a full queue since the last call. Resets to 0.
       *
       * The drop is counted on the Bluetooth RX thread (an outsider-controlled
       * rate) rather than logged there - see command_scanner.cpp.
       */
      uint32_t TakeDroppedCount();

      /**
       * @brief Adverts and single 128-bit UUID adverts received since the last
       *        call. Bench diagnostics only (CONFIG_MFS_SCAN_DIAG); counted on the
       *        Bluetooth RX thread and read from the main loop.
       */
      void TakeReceptionCounts(uint32_t& adverts, uint32_t& uuids);

      /**
       * @brief Switch between the duty-cycled scan and a continuous one.
       *
       * Continuous while Arming or while an armed trigger is pending, so a
       * deactivate is heard within one advert rather than ~30 s. Battery life is
       * explicitly not a factor then. The cadence is decided by DesiredFastScan()
       * (scan_policy.hpp) and applied only by App::applyScanCadence().
       *
       * Records `fast` as the REQUESTED cadence regardless of outcome - see
       * IsAtRequestedCadence(). On failure to start at it, falls back to
       * restarting at the PREVIOUS (achieved) cadence rather than leaving the
       * scanner stopped outright; a mismatch between requested and achieved is
       * then what ServiceScan() corrects. If even the fallback fails the
       * scanner is left down - see IsScanning().
       *
       * @return 0 on success (including a no-op when already at this cadence);
       *         the original negative errno on failure, whether or not the
       *         fallback restart succeeded.
       */
      int SetFastScan(bool fast);

      /** @brief The cadence last REQUESTED via SetFastScan(), whether or not it was achieved. */
      bool IsFastRequested() const { return m_fast_requested; }

      /** @brief True only while a scan is confirmed running, AT ANY cadence. */
      bool IsScanning() const { return m_scanning; }

      /**
       * @brief Whether the scanner has been left down since the last call. Clears the latch.
       *
       * Set whenever SetFastScan() or ServiceScan() returns with the scanner not
       * running (the start and its fallback both failed) - not by the momentary
       * stop inside a cadence change that restarts successfully. STICKY until
       * read, so a loss that ServiceScan() heals before anyone checks
       * IsScanning() is still reported. Read by App::TakeScannerLost() for the
       * arming sequence's fail-safe.
       */
      bool TakeScanLost();

      /**
       * @brief True only while running AND at the cadence last requested.
       *
       * IsScanning() alone is not enough: a scan that is running but stuck at
       * the wrong cadence (a failed SetFastScan() whose fallback succeeded) can
       * still hear adverts, just not at the rate that was asked for - a silent
       * failure mode that is easy to mistake for full health. This is the
       * correct predicate for health checks and retry logic - see
       * App::serviceScanHealth().
       */
      bool IsAtRequestedCadence() const { return m_scanning && m_fast == m_fast_requested; }

      /**
       * @brief Restore the scan to the last requested cadence if it is not
       * already there - whether stopped outright or merely running at the
       * wrong (fallback) cadence.
       *
       * Call periodically from the main loop. This is what eventually
       * recovers from either a SetFastScan() that left the scanner down, or
       * one whose fallback left it running at the wrong cadence - see
       * SetFastScan() and IsAtRequestedCadence().
       *
       * @return 0 if already at the requested cadence or the restart
       *         succeeded; the negative errno from the failed attempt at the
       *         requested cadence otherwise, whether or not a further
       *         fallback restart succeeded.
       */
      int ServiceScan();

    private:
      bool m_started;

      // The cadence last ACHIEVED - what the radio is actually doing. May
      // differ from m_fast_requested after a failed SetFastScan() whose
      // fallback succeeded. See IsAtRequestedCadence().
      bool m_fast;

      // The cadence last REQUESTED via SetFastScan(), regardless of whether
      // it was achieved. See IsAtRequestedCadence() and ServiceScan().
      bool m_fast_requested;

      // True only while a scan is known to be running - see IsScanning().
      bool m_scanning;

      // Latched when an operation leaves the scanner down - see TakeScanLost().
      bool m_scan_lost;
  };

}
