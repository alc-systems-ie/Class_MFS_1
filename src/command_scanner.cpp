#include <cstring>

#include <zephyr/bluetooth/bluetooth.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/atomic.h>
#include <zephyr/sys/byteorder.h>

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

    // 0xFFFF is the reserved-for-test company identifier. A production build must use
    // an assigned Bluetooth SIG company ID.
    constexpr uint16_t M_COMPANY_ID_TEST { 0xFFFF };
    constexpr uint8_t M_MAGIC_0 { 'M' };
    constexpr uint8_t M_MAGIC_1 { 'F' };
    constexpr uint8_t M_PROTOCOL_VERSION { 0x01 };

    // One command per cooldown. The engineer's tool must advertise for longer than a
    // full scan period to be seen at all, which means a single press can otherwise
    // span two scan windows and toggle twice. The cooldown must therefore exceed the
    // tool's advertising burst — see tools/toggle_dongle.
    constexpr int64_t M_COMMAND_COOLDOWN_MS { 12000 };

    /** @brief Advertising payload, immediately following the AD type byte. */
    struct __packed CommandPayload
    {
        uint16_t companyId; // Little-endian on the wire.
        uint8_t magic[2];
        uint8_t version;
        uint8_t command;
    };

    atomic_t s_pending_command { 0 };
    int64_t s_last_accepted_ms { 0 };

    bool parseAdStructure(struct bt_data* data, void* userData)
    {
      CommandPayload payload {};

      ARG_UNUSED(userData);

      if (data->type != BT_DATA_MANUFACTURER_DATA || data->data_len < sizeof(CommandPayload)) {
        return true; // Keep walking the remaining AD structures.
      }

      memcpy(&payload, data->data, sizeof(payload));

      if (sys_le16_to_cpu(payload.companyId) != M_COMPANY_ID_TEST) { return true; }
      if (payload.magic[0] != M_MAGIC_0 || payload.magic[1] != M_MAGIC_1) { return true; }
      if (payload.version != M_PROTOCOL_VERSION) {
        LOG_WRN("Command payload version %u unsupported!", payload.version);
        return false;
      }

#if defined(CONFIG_MFS_INSECURE_TOGGLE)
      atomic_set(&s_pending_command, payload.command);
#else
      // No TAN validation is implemented yet, so without the bench gate a command
      // carries no authority and must be discarded rather than obeyed.
      LOG_WRN("Command 0x%02X discarded: CONFIG_MFS_INSECURE_TOGGLE is not enabled!", payload.command);
#endif
      return false; // Match found; stop walking.
    }

    void scanRecvCallback(const bt_addr_le_t* addr, int8_t rssi, uint8_t advType, struct net_buf_simple* buf)
    {
      int64_t now { k_uptime_get() };

      ARG_UNUSED(addr);
      ARG_UNUSED(advType);

      if (s_last_accepted_ms != 0 && (now - s_last_accepted_ms) < M_COMMAND_COOLDOWN_MS) { return; }

      bt_data_parse(buf, &parseAdStructure, nullptr);

      if (atomic_get(&s_pending_command) != 0) {
        s_last_accepted_ms = now;
        LOG_INF("Command received, RSSI %d dBm.", rssi);
      }
    }

  }

  CommandScanner::CommandScanner()
      : m_started(false)
  {}

  int CommandScanner::Start()
  {
    int result { bt_enable(nullptr) };

    const struct bt_le_scan_param scanParam {
      .type     = BT_LE_SCAN_TYPE_PASSIVE,
      .options  = BT_LE_SCAN_OPT_NONE,
      .interval = M_SCAN_INTERVAL_UNITS,
      .window   = M_SCAN_WINDOW_UNITS,
    };

    if (result < 0) {
      LOG_ERR("bt_enable failed: %d!", result);
      return result;
    }

    result = bt_le_scan_start(&scanParam, &scanRecvCallback);
    if (result < 0) {
      LOG_ERR("bt_le_scan_start failed: %d!", result);
      return result;
    }

    m_started = true;
    LOG_INF("Passive scan started: %u ms window every %u ms.", CONFIG_MFS_SCAN_WINDOW_MS, CONFIG_MFS_SCAN_PERIOD_MS);
    return 0;
  }

  CommandScanner::Command CommandScanner::TakePendingCommand()
  {
    atomic_val_t raw { atomic_set(&s_pending_command, 0) };

    if (raw == static_cast<atomic_val_t>(Command::ToggleArm)) { return Command::ToggleArm; }
    if (raw != 0) { LOG_WRN("Unknown command 0x%02X ignored!", static_cast<unsigned int>(raw)); }
    return Command::None;
  }

}
