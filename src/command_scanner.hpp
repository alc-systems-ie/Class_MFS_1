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
       * @brief Switch between the duty-cycled scan and a continuous one.
       *
       * Continuous while a trigger is pending, so a deactivate is heard within one
       * advert rather than ~30 s. Battery life is explicitly not a factor then.
       *
       * On failure to start at the requested cadence, falls back to restarting at
       * the PREVIOUS one rather than leaving the scanner stopped outright. If even
       * that fails the scanner is left down - see ServiceScan() and IsScanning().
       *
       * @return 0 on success (including a no-op when already at this cadence);
       *         the original negative errno on failure, whether or not the
       *         fallback restart succeeded.
       */
      int SetFastScan(bool fast);

      /** @brief True only while a scan is confirmed running. See ServiceScan(). */
      bool IsScanning() const { return m_scanning; }

      /**
       * @brief Retry starting the scan if it is not currently running.
       *
       * Call periodically from the main loop. A scan can be left stopped if
       * SetFastScan() fails at both the requested and the fallback cadence -
       * this is what eventually recovers it, at the last cadence requested.
       *
       * @return 0 if already running or the restart succeeded; negative errno
       *         from bt_le_scan_start() otherwise.
       */
      int ServiceScan();

    private:
      bool m_started;
      bool m_fast;

      // True only while a scan is known to be running - see IsScanning().
      bool m_scanning;
  };

}
