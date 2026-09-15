#include <zephyr/bluetooth/bluetooth.h>
#include <zephyr/logging/log.h>

#include "command_advertiser.hpp"
#include "mfs_protocol.hpp"

LOG_MODULE_REGISTER(command_advertiser, LOG_LEVEL_INF);

namespace alc
{

  CommandAdvertiser::CommandAdvertiser()
      : m_advertising(false)
      , m_stop_deadline_ms(0)
  {}

  void CommandAdvertiser::Burst(const uint8_t uuid[16], int64_t nowMs, int64_t durationMs)
  {
    const struct bt_data ad[] = {
      BT_DATA(BT_DATA_UUID128_ALL, uuid, protocol::M_UUID_BYTES),
    };
    int result { 0 };

    // A burst already running is stopped first, so a rehearsal immediately
    // followed by a live fire (or two fires in quick succession) never leaves
    // two adverts contending for the radio - bt_le_adv_start() would otherwise
    // simply fail with -EALREADY.
    if (m_advertising) { bt_le_adv_stop(); }

    result = bt_le_adv_start(BT_LE_ADV_NCONN, ad, ARRAY_SIZE(ad), nullptr, 0);
    if (result < 0) {
      LOG_ERR("Fire confirmation advert failed to start: %d!", result);
      m_advertising = false;
      return;
    }

    m_advertising      = true;
    m_stop_deadline_ms = nowMs + durationMs;
    LOG_INF("Fire confirmation advert started for %lld ms.", durationMs);
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

    LOG_INF("Fire confirmation advert stopped.");
  }

}
