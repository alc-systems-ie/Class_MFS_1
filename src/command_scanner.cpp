#include <cstring>

#include <zephyr/bluetooth/bluetooth.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/atomic.h>

#include "command_scanner.hpp"

LOG_MODULE_REGISTER(scanner, LOG_LEVEL_INF);

namespace alc
{

  namespace
  {

    // Scan interval and window are expressed in 0.625 ms units. The controller does
    // the duty cycling, so the SoC sleeps between windows with no software timer.
    constexpr uint16_t M_UNITS_PER_MS_NUM { 8 };
    constexpr uint16_t M_UNITS_PER_MS_DEN { 5 };
    constexpr uint16_t M_SCAN_INTERVAL_UNITS { CONFIG_MFS_SCAN_PERIOD_MS * M_UNITS_PER_MS_NUM / M_UNITS_PER_MS_DEN };
    constexpr uint16_t M_SCAN_WINDOW_UNITS { CONFIG_MFS_SCAN_WINDOW_MS * M_UNITS_PER_MS_NUM / M_UNITS_PER_MS_DEN };

    // Complete list of 128-bit service UUIDs. iOS cannot send manufacturer data at
    // all, so the payload travels as a service UUID - see the design spec section 3.
    constexpr uint8_t M_AD_UUID128_ALL { 0x07 };

    // The main loop drains this every 100 ms. Eight distinct UUIDs in 100 ms is far
    // beyond any real radio environment around a covert sensor.
    constexpr size_t M_QUEUE_DEPTH { 8 };

    // How many recently queued UUIDs are remembered for repeat suppression.
    constexpr uint8_t M_RECENT_COUNT { 8 };

    // A phone repeats the same advert roughly every 187 ms, so 2 s still collapses
    // a 30 s burst about 10:1. Expiring the entry (rather than remembering it
    // forever) means a command first seen while it could not yet be judged - the
    // clock invalid, a lockout, a transient persist failure - is re-offered within
    // 2 s instead of being suppressed for the rest of its burst.
    constexpr int64_t M_RECENT_TTL_MS { 2000 };

    K_MSGQ_DEFINE(s_candidates, sizeof(CommandScanner::Candidate), M_QUEUE_DEPTH, 1);

    // Counted, not logged - see CommandScanner::TakeDroppedCount(). Logging on
    // the Bluetooth RX thread at a rate an outsider controls, in immediate log
    // mode on a 2 KB stack, is not safe.
    atomic_t s_dropped { ATOMIC_INIT(0) };

    // Bench reception counters - see CommandScanner::TakeReceptionCounts().
    atomic_t s_adverts_received { ATOMIC_INIT(0) };
    atomic_t s_uuids_received { ATOMIC_INIT(0) };

    struct RecentEntry
    {
        uint8_t bytes[protocol::M_UUID_BYTES];
        int64_t seenMs;
    };

    // Touched only from the Bluetooth RX thread, so no lock is needed.
    RecentEntry s_recent[M_RECENT_COUNT] {};
    uint8_t s_recent_next { 0 };
    uint8_t s_recent_filled { 0 };

    bool seenRecently(const uint8_t* bytes, int64_t nowMs)
    {
      for (uint8_t index = 0; index < s_recent_filled; index++) {
        if (nowMs - s_recent[index].seenMs >= M_RECENT_TTL_MS) { continue; }
        if (memcmp(s_recent[index].bytes, bytes, protocol::M_UUID_BYTES) == 0) { return true; }
      }
      return false;
    }

    void remember(const uint8_t* bytes, int64_t nowMs)
    {
      memcpy(s_recent[s_recent_next].bytes, bytes, protocol::M_UUID_BYTES);
      s_recent[s_recent_next].seenMs = nowMs;
      s_recent_next                  = static_cast<uint8_t>((s_recent_next + 1) % M_RECENT_COUNT);
      if (s_recent_filled < M_RECENT_COUNT) { s_recent_filled++; }
    }

    bool parseAdStructure(struct bt_data* data, void* userData)
    {
      CommandScanner::Candidate candidate {};
      int64_t nowMs { 0 };

      ARG_UNUSED(userData);

      // Exactly one 128-bit UUID. A list of several is not our phone, which never
      // advertises anything else.
      if (data->type != M_AD_UUID128_ALL || data->data_len != protocol::M_UUID_BYTES) { return true; }

      atomic_inc(&s_uuids_received);

      nowMs = k_uptime_get();
      if (seenRecently(data->data, nowMs)) { return false; }

      memcpy(candidate.bytes, data->data, protocol::M_UUID_BYTES);
      if (k_msgq_put(&s_candidates, &candidate, K_NO_WAIT) != 0) {
        // Dropped, not remembered - so a later copy of the same advert can still
        // get in once the loop has drained the queue.
        atomic_inc(&s_dropped);
        return false;
      }

      remember(data->data, nowMs);
      return false;
    }

    void scanRecvCallback(const bt_addr_le_t* addr, int8_t rssi, uint8_t advType, struct net_buf_simple* buf)
    {
      ARG_UNUSED(addr);
      ARG_UNUSED(rssi);
      ARG_UNUSED(advType);

      atomic_inc(&s_adverts_received);
      bt_data_parse(buf, &parseAdStructure, nullptr);
    }

    // One place that builds the scan parameters and starts the scan, so
    // Start(), SetFastScan() and ServiceScan() cannot disagree on how.
    int startScan(bool fast)
    {
      const struct bt_le_scan_param scanParam {
        .type     = BT_LE_SCAN_TYPE_PASSIVE,
        .options  = BT_LE_SCAN_OPT_NONE,
        .interval = fast ? M_SCAN_WINDOW_UNITS : M_SCAN_INTERVAL_UNITS,
        .window   = M_SCAN_WINDOW_UNITS,
      };
      int result { bt_le_scan_start(&scanParam, &scanRecvCallback) };

      // A prior stopScan() that failed to actually stop the controller (see
      // stopScan()) leaves a scan already running when this is called to
      // start the next one, and bt_le_scan_start() reports that as
      // -EALREADY rather than success. A scan IS running in that case - just
      // not provably at the cadence just requested - so this is treated as
      // success rather than a permanent, unrecoverable "scanner is down".
      if (result == -EALREADY) { return 0; }
      return result;
    }

    // One place that stops the scan and reports anything unexpected, so
    // SetFastScan() and ServiceScan() cannot disagree on how.
    void stopScan()
    {
      int result { bt_le_scan_stop() };

      // -EALREADY just means there was nothing running to stop - not worth a
      // log. Anything else means the controller may still have a scan
      // running at the cadence this call was meant to end; startScan()'s own
      // -EALREADY handling (above) is what recovers from that.
      if (result < 0 && result != -EALREADY) { LOG_WRN("bt_le_scan_stop failed: %d!", result); }
    }

  }

  CommandScanner::CommandScanner()
      : m_started(false)
      , m_fast(false)
      , m_fast_requested(false)
      , m_scanning(false)
  {}

  int CommandScanner::Start()
  {
    int result { bt_enable(nullptr) };

    if (result < 0) {
      LOG_ERR("bt_enable failed: %d!", result);
      return result;
    }

    result = startScan(false);
    if (result < 0) {
      LOG_ERR("bt_le_scan_start failed: %d!", result);
      return result;
    }

    m_started  = true;
    m_scanning = true;
    LOG_INF("Passive scan started: %u ms window every %u ms.", CONFIG_MFS_SCAN_WINDOW_MS, CONFIG_MFS_SCAN_PERIOD_MS);
    return 0;
  }

  bool CommandScanner::TakeCandidate(Candidate& out)
  {
    return k_msgq_get(&s_candidates, &out, K_NO_WAIT) == 0;
  }

  uint32_t CommandScanner::TakeDroppedCount()
  {
    return static_cast<uint32_t>(atomic_set(&s_dropped, 0));
  }

  void CommandScanner::TakeReceptionCounts(uint32_t& adverts, uint32_t& uuids)
  {
    adverts = static_cast<uint32_t>(atomic_set(&s_adverts_received, 0));
    uuids   = static_cast<uint32_t>(atomic_set(&s_uuids_received, 0));
  }

  int CommandScanner::SetFastScan(bool fast)
  {
    int result { 0 };
    int fallbackResult { 0 };

    // Recorded regardless of outcome below - see IsAtRequestedCadence() and
    // ServiceScan(), which is what reconciles m_fast with this if the
    // requested cadence is not achieved here.
    m_fast_requested = fast;

    if (!m_started) { return 0; }
    if (fast == m_fast && m_scanning) { return 0; }

    // Unconditional: a scan that stopScan() fails to actually stop is still
    // treated as not-yet-at-the-new-cadence here, so m_scanning tracks intent
    // immediately rather than only on the success path. startScan()'s
    // -EALREADY handling is what recovers if the controller disagrees.
    stopScan();
    m_scanning = false;

    result = startScan(fast);
    if (result < 0) {
      LOG_ERR("Failed to change scan cadence to %s: %d!", fast ? "CONTINUOUS" : "duty-cycled", result);

      // Fall back to restarting at the cadence that was running before,
      // rather than leave the scanner stopped outright on a single failed
      // start. m_fast is deliberately NOT updated to `fast` here, so
      // IsAtRequestedCadence() correctly reports the mismatch and
      // ServiceScan() (see App::serviceScanHealth()) keeps retrying for it.
      fallbackResult = startScan(m_fast);
      m_scanning     = (fallbackResult == 0);
      if (!m_scanning) { LOG_ERR("Scanner fallback restart also failed: %d - scanner is DOWN!", fallbackResult); }
      return result;
    }

    m_fast     = fast;
    m_scanning = true;
    LOG_INF("Scan cadence now %s.", fast ? "CONTINUOUS" : "duty-cycled");
    return 0;
  }

  int CommandScanner::ServiceScan()
  {
    int result { 0 };
    int fallbackResult { 0 };

    if (!m_started) { return 0; }
    if (m_scanning && m_fast == m_fast_requested) { return 0; }

    // Stop first if a scan is running at the wrong cadence; nothing to stop
    // if a previous attempt already left it down.
    if (m_scanning) {
      stopScan();
      m_scanning = false;
    }

    result = startScan(m_fast_requested);
    if (result == 0) {
      m_fast     = m_fast_requested;
      m_scanning = true;
      LOG_WRN("Scanner restored at the requested cadence.");
      return 0;
    }

    // Fall back to the cadence last known to work, rather than leave the
    // scanner stopped outright on a single failed retry.
    fallbackResult = startScan(m_fast);
    m_scanning     = (fallbackResult == 0);
    if (!m_scanning) { LOG_ERR("Scanner fallback restart also failed: %d - scanner is DOWN!", fallbackResult); }
    return result;
  }

}
