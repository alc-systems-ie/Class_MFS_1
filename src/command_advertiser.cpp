#include <zephyr/bluetooth/bluetooth.h>
#include <zephyr/bluetooth/hci.h>
#include <zephyr/bluetooth/hci_vs.h>
#include <zephyr/sys/byteorder.h>
#include <zephyr/logging/log.h>

#include "command_advertiser.hpp"
#include "mfs_protocol.hpp"

LOG_MODULE_REGISTER(command_advertiser, LOG_LEVEL_INF);

namespace alc
{

  namespace
  {
    // 20 ms in 0.625 ms units: the fastest interval a non-connectable advert may
    // use, so a 100 ms report burst carries about five adverts.
    constexpr uint32_t M_REPORT_ADV_INTERVAL_UNITS { 32 };
  }

  CommandAdvertiser::CommandAdvertiser()
      : m_advertising(false)
      , m_stop_deadline_ms(0)
  {}

  void CommandAdvertiser::setTxPower(int8_t txPowerDbm)
  {
    constexpr uint16_t M_ADV_HANDLE { 0 };
    struct bt_hci_cp_vs_write_tx_power_level* command { nullptr };
    struct bt_hci_rp_vs_write_tx_power_level* response { nullptr };
    struct net_buf* buffer { nullptr };
    struct net_buf* reply { nullptr };
    int result { 0 };

    buffer = bt_hci_cmd_alloc(K_NO_WAIT);
    if (buffer == nullptr) {
      LOG_WRN("Report advert power not set (%d) - previous power.", -ENOMEM);
      return;
    }

    command                 = static_cast<struct bt_hci_cp_vs_write_tx_power_level*>(net_buf_add(buffer, sizeof(*command)));
    command->handle         = sys_cpu_to_le16(M_ADV_HANDLE);
    command->handle_type    = BT_HCI_VS_LL_HANDLE_TYPE_ADV;
    command->tx_power_level = txPowerDbm;

    result = bt_hci_cmd_send_sync(BT_HCI_OP_VS_WRITE_TX_POWER_LEVEL, buffer, &reply);
    if (result < 0) {
      LOG_WRN("Report advert power not set (%d) - previous power.", result);
      return;
    }

    response = reinterpret_cast<struct bt_hci_rp_vs_write_tx_power_level*>(reply->data);
    LOG_INF("Report advert at %d dBm (controller selected %d dBm).", txPowerDbm, response->selected_tx_power);
    net_buf_unref(reply);
  }

  int CommandAdvertiser::Burst(const uint8_t uuid[16], int64_t nowMs, int64_t durationMs, int8_t txPowerDbm)
  {
    const struct bt_data ad[] = {
      BT_DATA(BT_DATA_UUID128_ALL, uuid, protocol::M_UUID_BYTES),
    };
    int result { 0 };

    // A burst already running is stopped first, so a report sent while another
    // is still on air (a fire report cutting a low-battery burst short) never
    // leaves two adverts contending for the radio - bt_le_adv_start() would
    // otherwise simply fail with -EALREADY.
    if (m_advertising) { bt_le_adv_stop(); }

    // Power is set both before and after the start: the SDC ignores the handle for legacy advertising, so the first call targets the single
    // legacy advertiser, and the second covers a controller that resets power on LE Set Advertising Parameters. Neither changes the result.
    setTxPower(txPowerDbm);

    // Non-connectable, non-scannable, no identity (BT_LE_ADV_OPT_NONE).
    result = bt_le_adv_start(BT_LE_ADV_PARAM(BT_LE_ADV_OPT_NONE, M_REPORT_ADV_INTERVAL_UNITS, M_REPORT_ADV_INTERVAL_UNITS, nullptr), ad,
                             ARRAY_SIZE(ad), nullptr, 0);
    if (result < 0) {
      LOG_ERR("Report advert failed to start: %d!", result);
      m_advertising = false;
      return result;
    }

    m_advertising      = true;
    m_stop_deadline_ms = nowMs + durationMs;
    setTxPower(txPowerDbm);
    LOG_INF("Report advert started for %lld ms.", durationMs);
    return 0;
  }

  void CommandAdvertiser::Service(int64_t nowMs)
  {
    int result { 0 };

    if (!m_advertising) { return; }
    if (nowMs < m_stop_deadline_ms) { return; }

    result        = bt_le_adv_stop();
    m_advertising = false;
    if (result < 0) {
      LOG_WRN("bt_le_adv_stop failed: %d!", result);
      return;
    }

    LOG_INF("Report advert stopped.");
  }

}
